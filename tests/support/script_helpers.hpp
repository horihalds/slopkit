#pragma once

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "script/engine.hpp"
#include "script/types.hpp"

namespace
{
    using slopkit::script::Engine;
    using slopkit::script::EngineConfig;
    using slopkit::script::MemoryApi;
    using slopkit::script::RunResult;

    // The base address an in-memory fake target answers from.
    inline constexpr std::uint64_t kScriptBase = 0x1000;

    // A MemoryApi over an owned buffer, so the engine can be exercised without a
    // session. Reads and writes outside the buffer fail with the same kind of
    // message the worker's `AccessError` text produces.
    struct FakeMemory
    {
        std::vector<std::byte> bytes;
        std::size_t            pointer_size {8};

        explicit FakeMemory(std::size_t size = 0x100) : bytes(size) {}

        [[nodiscard]] std::uint64_t address(std::size_t offset) const
        {
            return kScriptBase + offset;
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
                if (address < kScriptBase || address - kScriptBase + size > bytes.size())
                {
                    return std::unexpected(std::string {"address is not mapped"});
                }
                const auto offset = static_cast<std::size_t>(address - kScriptBase);
                return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
            };
            memory.write = [this](std::uint64_t              address,
                                  std::span<const std::byte> data) -> std::expected<void, std::string>
            {
                if (address < kScriptBase || address - kScriptBase + data.size() > bytes.size())
                {
                    return std::unexpected(std::string {"address is not mapped"});
                }
                const auto offset = static_cast<std::size_t>(address - kScriptBase);
                std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                return {};
            };
            return memory;
        }
    };

    // An engine over a fake target, ready to run chunks.
    struct ScriptFixture
    {
        FakeMemory fake;
        Engine     engine;

        explicit ScriptFixture(std::size_t size = 0x100, EngineConfig config = {})
            : fake(size), engine(fake.api(), config)
        {
        }
    };
} // namespace
