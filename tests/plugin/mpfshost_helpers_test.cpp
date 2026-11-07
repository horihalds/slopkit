#include <catch2/catch.hpp>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

#include "plugin/plugin_api.h"
#include "plugins/mpfshost/mpfshost_helpers.hpp"

namespace
{
    using slopkit::plugins::mpfshost::is_main_module;
    using slopkit::plugins::mpfshost::status_for_client_error;
    using slopkit::plugins::mpfshost::TargetTable;

    class EnvGuard
    {
    public:
        explicit EnvGuard(const char* name) : name_(name)
        {
            if (const char* value = std::getenv(name); value != nullptr)
            {
                saved_ = value;
            }
        }

        EnvGuard(const EnvGuard&)            = delete;
        EnvGuard& operator=(const EnvGuard&) = delete;

        ~EnvGuard()
        {
            if (saved_)
            {
                ::setenv(name_, saved_->c_str(), 1);
            }
            else
            {
                ::unsetenv(name_);
            }
        }

    private:
        const char*                name_;
        std::optional<std::string> saved_;
    };
}

TEST_CASE("the mpfshost target table keeps a stable synthetic pid", "[mpfshost]")
{
    TargetTable table;

    const auto first  = table.remember({"win11", 1234, "notepad.exe", false});
    const auto same   = table.remember({"win11", 1234, "notepad.exe", true});
    const auto other  = table.remember({"win10", 1234, "notepad.exe", false});
    const auto second = table.remember({"win11", 5678, "calc.exe", false});

    CHECK(first == same);
    CHECK(first != other);
    CHECK(first != second);
    CHECK(first >= 0x80000000u);

    const auto guest = table.lookup(first);
    REQUIRE(guest.has_value());
    CHECK(guest->target == "win11");
    CHECK(guest->pid == 1234);
    CHECK(guest->name == "notepad.exe");
    CHECK(guest->wow64);

    CHECK_FALSE(table.lookup(1).has_value());
}

TEST_CASE("the mpfshost PTE flags map onto region permissions", "[mpfshost]")
{
    const auto executable = slopkit::plugins::mpfshost::classify_pte_flags(0x4);
    CHECK(executable.readable);
    CHECK_FALSE(executable.writable);
    CHECK(executable.executable);

    const auto writable = slopkit::plugins::mpfshost::classify_pte_flags(0x8000000000000006ull);
    CHECK(writable.readable);
    CHECK(writable.writable);
    CHECK_FALSE(writable.executable);

    const auto read_only = slopkit::plugins::mpfshost::classify_pte_flags(0x8000000000000004ull);
    CHECK(read_only.readable);
    CHECK_FALSE(read_only.writable);
    CHECK_FALSE(read_only.executable);
}

TEST_CASE("the mpfshost main module match ignores a path and the exe suffix", "[mpfshost]")
{
    CHECK(is_main_module("notepad.exe", "notepad.exe"));
    CHECK(is_main_module("\\Windows\\System32\\notepad.exe", "notepad.exe"));
    CHECK(is_main_module("notepad.exe", "Notepad.EXE"));
    CHECK(is_main_module("C:/Windows/notepad.exe", "notepad"));
    CHECK_FALSE(is_main_module("kernel32.dll", "notepad.exe"));
    CHECK_FALSE(is_main_module({}, "notepad.exe"));
}

TEST_CASE("the mpfshost client errors map onto the plugin status codes", "[mpfshost]")
{
    CHECK(status_for_client_error("AccessDenied: access policy") == SLOPKIT_ERR_PERMISSION_DENIED);
    CHECK(status_for_client_error("access denied: uid 1000") == SLOPKIT_ERR_PERMISSION_DENIED);
    CHECK(status_for_client_error("TargetNotFound: win11") == SLOPKIT_ERR_NOT_FOUND);
    CHECK(status_for_client_error("connect failed: no such file") == SLOPKIT_ERR_NOT_FOUND);
    CHECK(status_for_client_error("InvalidRange: bad range") == SLOPKIT_ERR_INVALID_ARGUMENT);
    CHECK(status_for_client_error("MalformedPayload") == SLOPKIT_ERR_INVALID_ARGUMENT);
    CHECK(status_for_client_error("Internal: MemProcFS failed") == SLOPKIT_ERR_IO);
}

TEST_CASE("the mpfshost socket follows the environment override", "[mpfshost]")
{
    EnvGuard guard {"SLOPKIT_MPFSHOST_SOCKET"};

    ::unsetenv("SLOPKIT_MPFSHOST_SOCKET");
    CHECK(slopkit::plugins::mpfshost::socket_path() == "/run/mpfshost.sock");

    ::setenv("SLOPKIT_MPFSHOST_SOCKET", "/tmp/mpfshost.sock", 1);
    CHECK(slopkit::plugins::mpfshost::socket_path() == "/tmp/mpfshost.sock");
}
