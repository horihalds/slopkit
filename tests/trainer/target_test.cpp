#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "process/types.hpp"
#include "scan/types.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/scripts.hpp"
#include "trainer/runtime/target.hpp"

namespace
{
    using slopkit::process::ModuleInfo;
    using slopkit::process::owning_module_name;
    using slopkit::scan::ValueType;
    using slopkit::trainer::attach;
    using slopkit::trainer::AttachRequest;
    using slopkit::trainer::LiveEntry;
    using slopkit::trainer::Payload;
    using slopkit::trainer::PayloadEntry;
    using slopkit::trainer::PayloadScript;
    using slopkit::trainer::Resolver;
    using slopkit::trainer::ScriptHost;
    using slopkit::trainer::Target;

    constexpr std::string_view kPatternText {"de ad be ef"};

    ModuleInfo module(std::string name, std::uint64_t base, std::uint64_t size)
    {
        ModuleInfo info;
        info.base = base;
        info.size = size;
        info.name = std::move(name);
        return info;
    }

    // The layout a Wine or Proton PE image has in `/proc/<pid>/maps`: the image's
    // own sections carry its path while the executable page a script has to find
    // its pattern in is anonymous. `hole()` is that page.
    class ScratchImage
    {
    public:
        explicit ScratchImage(std::string_view file)
        {
            const std::filesystem::path directory = std::filesystem::path {SLOPKIT_TMP_DIR} / "trainer_target_test";
            std::error_code             error;
            std::filesystem::remove_all(directory, error);
            std::filesystem::create_directories(directory, error);

            path_ = directory / file;
            page_ = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            size_ = page_ * 3;

            const int descriptor = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
            REQUIRE(descriptor >= 0);
            REQUIRE(::ftruncate(descriptor, static_cast<off_t>(size_)) == 0);

            mapping_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE, descriptor, 0);
            ::close(descriptor);
            REQUIRE(mapping_ != MAP_FAILED);

            void* anonymous = ::mmap(reinterpret_cast<void*>(static_cast<std::uintptr_t>(hole())),
                                     page_,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                     -1,
                                     0);
            REQUIRE(anonymous == reinterpret_cast<void*>(static_cast<std::uintptr_t>(hole())));
            const unsigned char pattern[] {0xde, 0xad, 0xbe, 0xef};
            std::memcpy(anonymous, pattern, sizeof(pattern));
        }

        ScratchImage(const ScratchImage&)            = delete;
        ScratchImage& operator=(const ScratchImage&) = delete;

        ~ScratchImage()
        {
            if (mapping_ != MAP_FAILED)
            {
                ::munmap(mapping_, size_);
            }
        }

        [[nodiscard]] std::uint64_t base() const
        {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mapping_));
        }

        [[nodiscard]] std::uint64_t hole() const
        {
            return base() + page_;
        }

        [[nodiscard]] std::string name() const
        {
            return path_.filename().string();
        }

    private:
        std::filesystem::path path_;
        std::size_t           page_ {0};
        std::size_t           size_ {0};
        void*                 mapping_ {MAP_FAILED};
    };

    Payload payload_locating(const ScratchImage& image)
    {
        PayloadScript script;
        script.description = "Locate the module";
        script.source      = std::format(R"(
function activate()
    local found, address = aobscan("hit", "{}", "{}")
    if not found then
        return false, "pattern not found"
    end
    ssymbol("hit", address)
end
)",
                                         kPatternText,
                                         image.name());

        PayloadEntry entry;
        entry.description = "Found";
        entry.type        = ValueType::int32;
        entry.expression  = "hit";
        entry.bytes.assign(4, std::byte {0});

        Payload payload;
        payload.pointer_size = 8;
        payload.process_name = image.name();
        payload.entries.push_back(std::move(entry));
        payload.scripts.push_back(std::move(script));
        return payload;
    }
}

TEST_CASE("a module owns the addresses its image span covers", "[trainer][target]")
{
    const std::vector<ModuleInfo> modules {module("game.exe", 0x1000, 0x2000), module("gamedll.dll", 0x10000, 0x4000)};

    CHECK(owning_module_name(0x1000, modules) == "game.exe");
    CHECK(owning_module_name(0x2fff, modules) == "game.exe");
    CHECK(owning_module_name(0x11000, modules) == "gamedll.dll");
    CHECK(owning_module_name(0x3000, modules).empty());
    CHECK(owning_module_name(0x1000, std::vector<ModuleInfo> {}).empty());

    SECTION("a zero-sized image owns nothing")
    {
        const std::vector<ModuleInfo> empty {module("empty.so", 0x1000, 0)};
        CHECK(owning_module_name(0x1000, empty).empty());
    }

    SECTION("the lowest-based image wins when spans overlap")
    {
        // What a Wine/Proton target looks like: the guest PE image's span wraps the
        // anonymous page its code section was mapped as, and that page is a module
        // of its own.
        const std::vector<ModuleInfo> nested {module("[anon]", 0x2000, 0x1000), module("game.dll", 0x1000, 0x3000)};
        CHECK(owning_module_name(0x2000, nested) == "game.dll");
    }
}

// A script's module-scoped `aobscan` has to see the anonymous page a Wine/Proton
// PE image maps its code as, or the trainer reports the pattern missing where
// slopkit finds it. The scratch image here is that layout, attached to in-process
// so the whole path — attach, the region list, the script host — is exercised.
TEST_CASE("a trainer's own target names a region after the module that owns it", "[trainer][target]")
{
    ScratchImage image {"slopkit-region-probe.dll"};

    const auto attached = attach(AttachRequest {.pid = static_cast<std::uint32_t>(::getpid()), .name = {}});
    REQUIRE(attached.has_value());
    const Target& target = **attached;

    const auto holed = std::ranges::find_if(target.regions(),
                                            [&image](const auto& region)
                                            {
                                                return region.start == image.hole();
                                            });
    REQUIRE(holed != target.regions().end());
    CHECK(holed->readable);
    CHECK(holed->name == image.name());

    const Payload payload = payload_locating(image);
    ScriptHost    host {target, payload, 250};
    Resolver      resolver {target, 8};
    LiveEntry     live;

    resolver.set_symbols(host.symbols());
    resolver.refresh(payload.entries.front(), live);
    CHECK_FALSE(live.resolved);

    const auto activated = host.activate(0);
    REQUIRE(activated.has_value());
    CHECK(host.states().front().active);

    resolver.set_symbols(host.symbols());
    resolver.refresh(payload.entries.front(), live);
    REQUIRE(live.resolved);
    CHECK(live.address == image.hole());
    CHECK(live.bytes
          == std::vector<std::byte> {std::byte {0xde}, std::byte {0xad}, std::byte {0xbe}, std::byte {0xef}});
}
