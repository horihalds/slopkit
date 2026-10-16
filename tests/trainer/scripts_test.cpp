#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "reclass/serialize.hpp"
#include "scan/types.hpp"
#include "support/script_helpers.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/scripts.hpp"
#include "trainer/runtime/target.hpp"

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::trainer::LiveEntry;
    using slopkit::trainer::Module;
    using slopkit::trainer::Payload;
    using slopkit::trainer::PayloadClass;
    using slopkit::trainer::PayloadEntry;
    using slopkit::trainer::PayloadScript;
    using slopkit::trainer::Region;
    using slopkit::trainer::Resolver;
    using slopkit::trainer::ScriptHost;
    using slopkit::trainer::Target;

    constexpr std::uint64_t kBase {0x1000};

    // A target the script host can act on: one readable and writable mapping plus
    // the module an expression can name. It is deliberately the whole of what the
    // host's memory API touches.
    class FakeTarget : public Target
    {
    public:
        explicit FakeTarget(std::size_t size = 0x100) : bytes_(size, std::byte {0})
        {
            regions_.push_back(Region {kBase, kBase + size, true, true, "test.so"});
            modules_.push_back(Module {"test.so", kBase});
        }

        [[nodiscard]] std::uint32_t pid() const override
        {
            return 4242;
        }

        [[nodiscard]] std::uint32_t pointer_size() const override
        {
            return 8;
        }

        [[nodiscard]] const std::vector<Module>& modules() const override
        {
            return modules_;
        }

        [[nodiscard]] const std::vector<Region>& regions() const override
        {
            return regions_;
        }

        [[nodiscard]] std::expected<std::vector<std::byte>, std::string> read(std::uint64_t address,
                                                                              std::size_t   size) const override
        {
            if (!mapped(address, size))
            {
                return std::unexpected(std::string {"address is not mapped"});
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            return std::vector<std::byte>(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                                          bytes_.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }

        [[nodiscard]] std::expected<void, std::string> write(std::uint64_t              address,
                                                             std::span<const std::byte> bytes) const override
        {
            if (!mapped(address, bytes.size()))
            {
                return std::unexpected(std::string {"address is not mapped"});
            }
            const auto offset = static_cast<std::size_t>(address - kBase);
            std::copy(bytes.begin(), bytes.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
            return {};
        }

        template<typename T>
        void put(std::size_t offset, T value)
        {
            std::memcpy(bytes_.data() + offset, &value, sizeof(T));
        }

        [[nodiscard]] std::uint32_t u32_at(std::size_t offset) const
        {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes_.data() + offset, sizeof(value));
            return value;
        }

    private:
        [[nodiscard]] bool mapped(std::uint64_t address, std::size_t size) const
        {
            return address >= kBase && address - kBase + size <= bytes_.size();
        }

        mutable std::vector<std::byte> bytes_;
        std::vector<Module>            modules_;
        std::vector<Region>            regions_;
    };

    Payload payload_with(std::vector<PayloadScript> scripts)
    {
        Payload payload;
        payload.pointer_size = 8;
        payload.process_name = "practice";
        payload.scripts      = std::move(scripts);
        return payload;
    }

    PayloadEntry int32_entry(std::string expression)
    {
        PayloadEntry entry;
        entry.description = "Health";
        entry.type        = ValueType::int32;
        entry.expression  = std::move(expression);
        entry.bytes.assign(4, std::byte {0});
        return entry;
    }
}

TEST_CASE("a script runs only when it is activated", "[trainer][scripts]")
{
    FakeTarget target;
    ScriptHost host {
        target,
        payload_with({PayloadScript {"Armour", R"(
function activate()
    mem.write(0x1000, "u32", 1234)
end
)"}}
        ),
        250
    };

    REQUIRE(host.size() == 1);
    CHECK_FALSE(host.states().front().active);
    CHECK(target.u32_at(0) == 0);

    REQUIRE(host.activate(0).has_value());
    CHECK(host.states().front().active);
    CHECK_FALSE(host.states().front().failed);
    CHECK(target.u32_at(0) == 1234);

    // Activating twice is refused rather than running the hook a second time.
    CHECK_FALSE(host.activate(0).has_value());
}

TEST_CASE("deactivating a script runs its hook", "[trainer][scripts]")
{
    FakeTarget target;
    ScriptHost host {
        target,
        payload_with({PayloadScript {"Armour", R"(
function activate()
    mem.write(0x1000, "u8", 1)
end
function deactivate()
    mem.write(0x1000, "u8", 0)
end
)"}}
        ),
        250
    };

    REQUIRE(host.activate(0).has_value());
    CHECK(target.u32_at(0) == 1);

    REQUIRE(host.deactivate(0).has_value());
    CHECK_FALSE(host.states().front().active);
    CHECK(target.u32_at(0) == 0);

    // An idle script cannot be deactivated.
    CHECK_FALSE(host.deactivate(0).has_value());
}

TEST_CASE("a failing script stops alone", "[trainer][scripts]")
{
    FakeTarget target;
    ScriptHost host {
        target,
        payload_with({PayloadScript {"Armour", R"(
function activate()
    mem.write(0x1000, "u8", 1)
end
)"},
                      PayloadScript {"Refused", R"(
function activate()
    return false, "no target object"
end
)"},
                      PayloadScript {"Broken", "function activate( end"}}
        ),
        250
    };

    REQUIRE(host.size() == 3);
    REQUIRE(host.activate(0).has_value());

    const auto refused = host.activate(1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == "no target object");
    CHECK(host.states()[1].failed);
    CHECK(host.states()[1].message == "no target object");

    const auto broken = host.activate(2);
    REQUIRE_FALSE(broken.has_value());
    CHECK(host.states()[2].failed);
    CHECK_FALSE(host.states()[2].message.empty());

    // Neither failure touched the script that works.
    CHECK(host.states()[0].active);
    CHECK_FALSE(host.states()[0].failed);
    CHECK(target.u32_at(0) == 1);
}

TEST_CASE("a symbol a script publishes resolves an entry until the script drops it", "[trainer][scripts]")
{
    FakeTarget target;
    target.put<std::uint32_t>(4, 0x2a2a2a2a);

    ScriptHost host {
        target,
        payload_with({PayloadScript {"Base", R"(
function activate()
    ssymbol("base", 0x1000)
end
function deactivate()
    usymbol("base")
end
)"}}
        ),
        250
    };

    const PayloadEntry entry = int32_entry("base+4");
    Resolver           resolver {target, 8};
    LiveEntry          live;

    resolver.set_symbols(host.symbols());
    resolver.refresh(entry, live);
    CHECK_FALSE(live.resolved);

    REQUIRE(host.activate(0).has_value());
    REQUIRE(host.symbols().size() == 1);
    resolver.set_symbols(host.symbols());
    resolver.refresh(entry, live);
    REQUIRE(live.resolved);
    CHECK(live.address == 0x1004);
    CHECK(live.bytes == std::vector<std::byte>(4, std::byte {0x2a}));

    REQUIRE(host.deactivate(0).has_value());
    CHECK(host.symbols().empty());
    resolver.set_symbols(host.symbols());
    resolver.refresh(entry, live);
    CHECK_FALSE(live.resolved);
}

TEST_CASE("an active script's update hook ticks on the interval it asks for", "[trainer][scripts]")
{
    FakeTarget target;
    ScriptHost host {
        target,
        payload_with({PayloadScript {"Tick", R"(
function activate()
    return true
end
function deactivate()
    return true
end
function update()
    update_interval(10)
    mem.write(0x1001, "u8", 2)
end
)"}}
        ),
        250
    };

    REQUIRE(host.activate(0).has_value());
    CHECK(host.states().front().interval_ms == 250);

    const auto start = std::chrono::steady_clock::now() + std::chrono::seconds {10};
    host.tick(start);
    CHECK(target.u32_at(1) == 2);
    // The interval the update hook returned is adopted from then on.
    CHECK(host.states().front().interval_ms == 10);

    target.put<std::uint8_t>(1, 0);
    host.tick(start + std::chrono::milliseconds {5});
    CHECK(target.u32_at(1) == 0);

    host.tick(start + std::chrono::milliseconds {10});
    CHECK(target.u32_at(1) == 2);

    // A deactivated script does not tick at all.
    REQUIRE(host.deactivate(0).has_value());
    target.put<std::uint8_t>(1, 0);
    host.tick(start + std::chrono::seconds {1});
    CHECK(target.u32_at(1) == 0);
}

TEST_CASE("a failing update stops that script's ticking", "[trainer][scripts]")
{
    FakeTarget target;
    ScriptHost host {
        target,
        payload_with({PayloadScript {"Tick", R"(
function activate()
    return true
end
function update()
    return false, "hp went negative"
end
)"}}
        ),
        250
    };

    REQUIRE(host.activate(0).has_value());
    host.tick(std::chrono::steady_clock::now() + std::chrono::seconds {10});

    CHECK_FALSE(host.states().front().active);
    CHECK(host.states().front().failed);
    CHECK(host.states().front().message == "hp went negative");
}

TEST_CASE("the trainer cannot allocate in the target", "[trainer][scripts]")
{
    FakeTarget target;
    ScriptHost host {
        target,
        payload_with({PayloadScript {"Cave", R"(
function activate()
    alloc("cave", 0x40)
end
)"}}
        ),
        250
    };

    const auto refused = host.activate(0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("cannot allocate in the target") != std::string::npos);
    CHECK(host.states().front().failed);
}

TEST_CASE("the payload's class project gives scripts typed views", "[trainer][scripts]")
{
    slopkit::reclass::Project project;
    build_structs_fixture(project);

    Payload payload = payload_with({
        PayloadScript {"Read health", R"(
function activate()
    print(structs.get("Player"):at(0x1000).health)
end
)"}
    });
    for (const auto& member : slopkit::reclass::to_members(project))
    {
        payload.classes.push_back(PayloadClass {member.name, member.text});
    }

    FakeTarget target;
    target.put<float>(0, 12.5F);

    ScriptHost host {target, payload, 250};
    CHECK(host.class_error().empty());

    REQUIRE(host.activate(0).has_value());
    REQUIRE(host.states().front().output.size() == 1);
    CHECK(host.states().front().output.front() == "12.5");
}

TEST_CASE("a class project that cannot be read leaves only structs unavailable", "[trainer][scripts]")
{
    Payload payload = payload_with({
        PayloadScript {"Armour", R"(
function activate()
    mem.write(0x1000, "u8", 3)
end
)"}
    });
    payload.classes.push_back(PayloadClass {"reclass/index.txt", "not an index"});

    FakeTarget target;
    ScriptHost host {target, payload, 250};
    CHECK_FALSE(host.class_error().empty());

    // Everything else still works.
    REQUIRE(host.activate(0).has_value());
    CHECK(target.u32_at(0) == 3);
}
