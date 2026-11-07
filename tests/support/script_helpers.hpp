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
#include "reclass/project.hpp"
#include "script/engine.hpp"
#include "script/symbols.hpp"
#include "script/types.hpp"
#include "support/reclass_helpers.hpp"

namespace
{
    using slopkit::script::Engine;
    using slopkit::script::EngineConfig;
    using slopkit::script::MemoryApi;
    using slopkit::script::MemoryRegion;
    using slopkit::script::RunResult;

    inline constexpr std::uint64_t kScriptBase = 0x1000;

    struct FakeMemory
    {
        std::vector<std::byte>                             bytes;
        std::size_t                                        pointer_size {8};
        std::uint64_t                                      base {kScriptBase};
        std::string                                        module_name {"test.so"};
        std::vector<MemoryRegion>                          regions;
        std::optional<std::string>                         regions_error;
        bool                                               can_allocate {true};
        std::optional<std::string>                         allocate_error;
        // When set, `alloc` returns this address (still recorded, so `dealloc`
        // can free it) instead of appending to the buffer, so a test can place a
        // mapping far away from the target.
        std::optional<std::uint64_t>                       forced_allocation;
        std::unordered_map<std::uint64_t, std::size_t>     allocations;
        std::vector<std::pair<std::uint64_t, std::size_t>> allocate_requests;
        std::vector<slopkit::expr::ModuleRef>              modules;
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

        template<typename T>
        void put_value(std::size_t offset, T value)
        {
            std::memcpy(bytes.data() + offset, &value, sizeof(T));
        }

        [[nodiscard]] std::vector<std::byte> take(std::size_t offset, std::size_t size) const
        {
            return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
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

    // The classes the `structs` tests share: a `Player` whose fields cover every
    // kind the binding maps, plus the `Weapon` and `Vec` classes the pointer and
    // inlined fields name. The offsets are the sequential ones the model lays
    // out, with no padding.
    inline void build_structs_fixture(slopkit::reclass::Project& project)
    {
        using slopkit::reclass::NodeKind;
        using slopkit::reclass::NodePtr;
        using slopkit::test::array_of;
        using slopkit::test::instance_array_of;
        using slopkit::test::node_of;
        using slopkit::test::union_of;
        using slopkit::test::wrapper_array_of;

        project.add_class("Weapon", "game.exe+1A2B40");
        project.classes().back().nodes.push_back(node_of(project, NodeKind::float32, "damage"));

        project.add_class("Vec", "game.exe+1A2B40");
        project.classes().back().nodes.push_back(node_of(project, NodeKind::float32, "x"));
        project.classes().back().nodes.push_back(node_of(project, NodeKind::float32, "y"));

        project.add_class("Player", "game.exe+1A2B40");
        auto&      player = project.classes().back();
        const auto add    = [&player](NodePtr node)
        {
            player.nodes.push_back(std::move(node));
        };

        add(node_of(project, NodeKind::float32, "health")); // 0
        add(node_of(project, NodeKind::int32, "ammo"));     // 4

        NodePtr weapon     = node_of(project, NodeKind::class_pointer, "weapon"); // 8
        weapon->class_name = "Weapon";
        add(std::move(weapon));

        NodePtr position     = node_of(project, NodeKind::class_instance, "pos"); // 16
        position->class_name = "Vec";
        add(std::move(position));

        add(array_of(project, 3, NodeKind::int32)); // 24
        player.nodes.back()->name = "items";

        NodePtr text = node_of(project, NodeKind::utf8_text, "name"); // 36
        text->set_extent(project, 8);
        add(std::move(text));

        add(node_of(project, NodeKind::boolean, "alive"));            // 44
        add(node_of(project, NodeKind::hex32, "flags"));              // 45
        add(node_of(project, NodeKind::pointer, "next"));             // 49
        add(union_of(project, {NodeKind::int32, NodeKind::float32})); // 57
        player.nodes.back()->name = "u";

        NodePtr slots = wrapper_array_of(project, "Weapon", 2); // 61
        slots->name   = "slots";
        add(std::move(slots));

        NodePtr targets = instance_array_of(project, "Weapon", 2); // 77
        targets->name   = "targets";
        add(std::move(targets));

        add(node_of(project, NodeKind::float64, "speed")); // 85

        NodePtr tag = node_of(project, NodeKind::utf16_text, "tag"); // 93
        tag->set_extent(project, 4);
        add(std::move(tag));

        add(node_of(project, NodeKind::vector3, "blob")); // 101
    }
}
