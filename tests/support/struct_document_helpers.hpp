#pragma once

#include <catch2/catch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "structs/definition.hpp"
#include "support/fake_process.hpp"
#include "ui/components/struct_document.hpp"
#include "ui/live_values.hpp"

namespace
{
    using slopkit::process::AccessWorker;
    using slopkit::process::AttachedTarget;
    using slopkit::structs::Structs;
    using slopkit::test::FakeAccess;
    using slopkit::test::FakeMemory;
    using slopkit::ui::components::StructDocument;

    constexpr std::uint64_t kStructBase = 0x1800;

    [[maybe_unused]] void attach_session(AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 42,
                                 "fake",
                                 [&](slopkit::process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && !attached)
        {
            worker.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(attached);
    }

    [[maybe_unused]] AttachedTarget attached_target()
    {
        AttachedTarget target;
        target.pid          = 42;
        target.session_live = true;
        target.method       = slopkit::process::AccessMethod::procfs_mem;
        return target;
    }

    [[maybe_unused]] void put_bytes(FakeMemory& memory, std::uint64_t address, std::span<const std::byte> data)
    {
        for (std::size_t index = 0; index < data.size(); ++index)
        {
            memory.bytes[address + index] = data[index];
        }
    }

    [[maybe_unused]] void put_u64(FakeMemory& memory, std::uint64_t address, std::uint64_t value)
    {
        std::vector<std::byte> bytes(8);
        std::memcpy(bytes.data(), &value, 8);
        put_bytes(memory, address, bytes);
    }

    [[maybe_unused]] void put_u16(FakeMemory& memory, std::uint64_t address, std::uint16_t value)
    {
        std::vector<std::byte> bytes(2);
        std::memcpy(bytes.data(), &value, 2);
        put_bytes(memory, address, bytes);
    }

    [[maybe_unused]] void put_u32(FakeMemory& memory, std::uint64_t address, std::uint32_t value)
    {
        std::vector<std::byte> bytes(4);
        std::memcpy(bytes.data(), &value, 4);
        put_bytes(memory, address, bytes);
    }

    [[maybe_unused]] void put_f32(FakeMemory& memory, std::uint64_t address, float value)
    {
        std::vector<std::byte> bytes(4);
        std::memcpy(bytes.data(), &value, 4);
        put_bytes(memory, address, bytes);
    }

    [[maybe_unused]] void put_f64(FakeMemory& memory, std::uint64_t address, double value)
    {
        std::vector<std::byte> bytes(8);
        std::memcpy(bytes.data(), &value, 8);
        put_bytes(memory, address, bytes);
    }

    [[maybe_unused]] void put_text(FakeMemory& memory, std::uint64_t address, std::string_view text)
    {
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            memory.bytes[address + index] = static_cast<std::byte>(text[index]);
        }
    }

    [[maybe_unused]] slopkit::ui::LiveReading
    reading(std::size_t id, const FakeMemory& memory, std::uint64_t base, std::size_t size)
    {
        slopkit::ui::LiveReading value;
        value.id       = id;
        value.readable = true;
        value.bytes.resize(size);
        for (std::size_t index = 0; index < size; ++index)
        {
            if (const auto entry = memory.bytes.find(base + index); entry != memory.bytes.end())
            {
                value.bytes[index] = entry->second;
            }
        }
        return value;
    }

    struct StructFixture
    {
        FakeAccess     access;
        AccessWorker   worker {access};
        AttachedTarget target = attached_target();
        Structs        definitions;
        StructDocument document {worker, target, definitions};

        StructFixture()
        {
            attach_session(worker);
            document.set_visible(true);
        }

        void drain()
        {
            while (worker.drain() > 0)
            {
            }
        }

        template<typename Predicate>
        bool pump(Predicate done)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                drain();
                if (done())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return done();
        }

        void pass()
        {
            drain();
            const auto                            requests = document.next_live_request();
            std::vector<slopkit::ui::LiveReading> readings;
            readings.reserve(requests.size());
            for (const slopkit::ui::LiveRequest& request : requests)
            {
                readings.push_back(reading(request.id, *access.memory, request.address, request.size));
            }
            document.apply_live_readings(readings);
        }
    };
}
