#pragma once

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "expr/resolver.hpp"
#include "script/engine.hpp"
#include "script/symbols.hpp"
#include "script/types.hpp"

namespace
{
    using slopkit::script::Engine;
    using slopkit::script::EngineConfig;
    using slopkit::script::MemoryApi;
    using slopkit::script::MemoryRegion;
    using slopkit::script::RunResult;

    // The base address an in-memory fake target answers from.
    inline constexpr std::uint64_t kScriptBase = 0x1000;

    // A MemoryApi over an owned buffer, so the engine can be exercised without a
    // session. Reads and writes outside the buffer fail with the same kind of
    // message the worker's `AccessError` text produces.
    struct FakeMemory
    {
        std::vector<std::byte>                             bytes;
        std::size_t                                        pointer_size {8};
        // The address the buffer answers from; a test that needs a 64-bit site
        // can raise it.
        std::uint64_t                                      base {kScriptBase};
        // The module name the default region reports, so a test can exercise the
        // module filter of `aobscan`.
        std::string                                        module_name {"test.so"};
        // When non-empty, overrides the default single readable region covering
        // the whole buffer.
        std::vector<MemoryRegion>                          regions;
        // When set, listing regions fails with this text.
        std::optional<std::string>                         regions_error;
        // Allocation seams for `alloc`/`dealloc`: a pool keyed by the address
        // the engine was handed, plus the module snapshot `expression` resolves
        // module names against.
        bool                                               can_allocate {true};
        std::optional<std::string>                         allocate_error;
        // When set, `alloc` returns this address (still recorded, so `dealloc`
        // can free it) instead of appending to the buffer, so a test can place a
        // mapping far away from the target.
        std::optional<std::uint64_t>                       forced_allocation;
        std::unordered_map<std::uint64_t, std::size_t>     allocations;
        std::vector<std::pair<std::uint64_t, std::size_t>> allocate_requests;
        std::vector<slopkit::expr::ModuleRef>              modules;
        // Validation seam for `validate`/`expression`: `can_validate` answers
        // directly (a range outside the buffer is invalid, not an error),
        // `validate_error` models a refused operation, and `validate_count`
        // records the calls.
        bool                                               can_validate {true};
        std::optional<std::string>                         validate_error;
        std::size_t                                        validate_count {0};
        // How many writes reached the seam, so a test can pin the two-write
        // activation; when `refuse_write_at` is set, a write to that address
        // fails instead.
        std::size_t                                        write_count {0};
        std::optional<std::uint64_t>                       refuse_write_at;

        explicit FakeMemory(std::size_t size = 0x100) : bytes(size) {}

        [[nodiscard]] std::uint64_t address(std::size_t offset) const
        {
            return base + offset;
        }

        void put(std::size_t offset, std::initializer_list<int> values)
        {
            std::size_t index = offset;
            for (const int value : values)
            {
                bytes[index++] = static_cast<std::byte>(value);
            }
        }

        void put_text(std::size_t offset, std::string_view text)
        {
            std::memcpy(bytes.data() + offset, text.data(), text.size());
        }

        [[nodiscard]] MemoryApi api()
        {
            MemoryApi memory;
            memory.pointer_size = [this]()
            {
                return pointer_size;
            };
            memory.read = [this](std::uint64_t address,
                                 std::size_t   size) -> std::expected<std::vector<std::byte>, std::string>
            {
                if (address < base || address - base + size > bytes.size())
                {
                    return std::unexpected(std::string {"address is not mapped"});
                }
                const auto offset = static_cast<std::size_t>(address - base);
                return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
            };
            memory.write = [this](std::uint64_t              address,
                                  std::span<const std::byte> data) -> std::expected<void, std::string>
            {
                ++write_count;
                if (refuse_write_at && address == *refuse_write_at)
                {
                    return std::unexpected(std::string {"write refused"});
                }
                if (address < base || address - base + data.size() > bytes.size())
                {
                    return std::unexpected(std::string {"address is not mapped"});
                }
                const auto offset = static_cast<std::size_t>(address - base);
                std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                return {};
            };
            memory.regions = [this]() -> std::expected<std::vector<MemoryRegion>, std::string>
            {
                if (regions_error)
                {
                    return std::unexpected(*regions_error);
                }
                if (!regions.empty())
                {
                    return regions;
                }
                MemoryRegion region;
                region.base     = base;
                region.size     = bytes.size();
                region.readable = true;
                region.module   = module_name;
                return std::vector<MemoryRegion> {region};
            };
            memory.allocate = [this](std::size_t size, std::uint64_t near) -> std::expected<std::uint64_t, std::string>
            {
                if (!can_allocate)
                {
                    return std::unexpected(std::string {"the target's plugin cannot allocate memory"});
                }
                if (allocate_error)
                {
                    return std::unexpected(*allocate_error);
                }
                if (forced_allocation)
                {
                    allocations[*forced_allocation] = size;
                    allocate_requests.emplace_back(near, size);
                    return *forced_allocation;
                }
                constexpr std::size_t page   = 0x1000;
                const std::size_t     offset = (bytes.size() + page - 1) / page * page;
                bytes.resize(offset + size, std::byte {0});
                const std::uint64_t address = base + offset;
                allocations[address]        = size;
                allocate_requests.emplace_back(near, size);
                return address;
            };
            memory.deallocate = [this](std::uint64_t address) -> std::expected<void, std::string>
            {
                const auto found = allocations.find(address);
                if (found == allocations.end())
                {
                    return std::unexpected(std::string {"not this session's allocation"});
                }
                allocations.erase(found);
                return {};
            };
            memory.modules = [this]() -> std::expected<std::vector<slopkit::expr::ModuleRef>, std::string>
            {
                return modules;
            };
            memory.validate = [this](std::uint64_t address, std::size_t size) -> std::expected<bool, std::string>
            {
                ++validate_count;
                if (!can_validate)
                {
                    return std::unexpected(std::string {"no target is attached"});
                }
                if (validate_error)
                {
                    return std::unexpected(*validate_error);
                }
                return address >= base && address - base + size <= bytes.size();
            };
            return memory;
        }
    };

    // An engine over a fake target, ready to run chunks, with the symbol seam
    // backed by a table the test can assert on.
    struct ScriptFixture
    {
        FakeMemory                   fake;
        slopkit::script::SymbolTable symbols;
        Engine                       engine;

        explicit ScriptFixture(std::size_t size = 0x100, EngineConfig config = {})
            : fake(size), engine(fake.api(), symbols.api(), config)
        {
        }
    };
} // namespace
