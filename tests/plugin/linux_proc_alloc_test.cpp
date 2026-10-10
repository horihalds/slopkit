#include <catch2/catch.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/linux/procfs.hpp"
#include "platform/linux/ptrace.hpp"
#include "plugin/plugin.hpp"
#include "plugin/plugin_host.hpp"
#include "process/types.hpp"

namespace
{
    // A shared counter, so the parent can tell the child kept running after an
    // allocation: the child's own copy of a private variable would be invisible.
    class AllocChild
    {
    public:
        AllocChild()
        {
            counter_ = static_cast<volatile std::uint64_t*>(
                ::mmap(nullptr, sizeof(std::uint64_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
            if (counter_ == MAP_FAILED)
            {
                counter_ = nullptr;
                return;
            }
            *counter_ = 0;

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                for (;;)
                {
                    *counter_ = *counter_ + 1;
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~AllocChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
            if (counter_ != nullptr)
            {
                ::munmap(const_cast<std::uint64_t*>(counter_), sizeof(std::uint64_t));
            }
        }

        AllocChild(const AllocChild&)            = delete;
        AllocChild& operator=(const AllocChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uint64_t counter() const
        {
            return counter_ != nullptr ? *counter_ : 0;
        }

    private:
        pid_t                   pid_ {-1};
        volatile std::uint64_t* counter_ {nullptr};
    };

    // Returns the region that fully contains [address, address + size), if one
    // exists. Linux `mmap` merges a new anonymous mapping into an adjacent one
    // when their protection and backing match, so two neighbouring pages can
    // appear as a single region that starts at either base; a mapping is found
    // by covering its range, never by a region starting exactly at its base.
    // Whole-page ranges stay exact: VMA boundaries are page-aligned, so a page
    // can never straddle two regions.
    std::optional<slopkit::platform::MappedRegion>
    region_covering(std::uint32_t pid, std::uint64_t address, std::uint64_t size)
    {
        for (const auto& region : slopkit::platform::read_maps(pid))
        {
            if (region.start <= address && address + size <= region.end)
            {
                return region;
            }
        }
        return std::nullopt;
    }

    bool mapped(std::uint32_t pid, std::uint64_t address, std::uint64_t size)
    {
        return region_covering(pid, address, size).has_value();
    }

    // True when `pid` has a region covering [address, address + size) that is
    // readable, writable and executable. Coverage, not an exact VMA start, is
    // used for the same coalescing reason as `region_covering`.
    bool mapped_rwx(std::uint32_t pid, std::uint64_t address, std::uint64_t size)
    {
        const auto region = region_covering(pid, address, size);
        return region.has_value() && region->readable && region->writable && region->executable;
    }

    template<typename Child>
    void wait_for_progress(const Child& child)
    {
        const std::uint64_t before = child.counter();
        for (int i = 0; i < 200; ++i)
        {
            if (child.counter() != before)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {5});
        }
        FAIL("the target stopped making progress");
    }

    // A child that maps its own anonymous RWX page and fills it with a decoy:
    // a `0F 05` pair followed by code that would write a marker into a shared
    // page. That mapping is the target's generated-code pattern the gadget scan
    // must never choose: the pair is not a real instruction boundary and the
    // bytes around it belong to the target, not to a module.
    class DecoyChild
    {
    public:
        static constexpr std::uint64_t kDecoyAddress = 0x10000000;
        static constexpr std::size_t   kDecoySize    = 0x1000;

        struct Shared
        {
            std::uint64_t marker {};
            std::uint64_t counter {};
        };

        DecoyChild()
        {
            shared_ = static_cast<Shared*>(
                ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
            if (shared_ == MAP_FAILED || shared_ == nullptr)
            {
                shared_ = nullptr;
                return;
            }
            shared_->marker  = 0;
            shared_->counter = 0;

            auto* decoy = static_cast<std::byte*>(::mmap(reinterpret_cast<void*>(kDecoyAddress),
                                                         kDecoySize,
                                                         PROT_READ | PROT_WRITE | PROT_EXEC,
                                                         MAP_PRIVATE | MAP_ANONYMOUS,
                                                         -1,
                                                         0));
            if (decoy == MAP_FAILED || decoy == nullptr)
            {
                return;
            }
            decoy_ = static_cast<std::byte*>(decoy);
            write_decoy();

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                for (;;)
                {
                    shared_->counter = shared_->counter + 1;
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~DecoyChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
            if (decoy_ != nullptr)
            {
                ::munmap(decoy_, kDecoySize);
            }
            if (shared_ != nullptr)
            {
                ::munmap(shared_, sizeof(Shared));
            }
        }

        DecoyChild(const DecoyChild&)            = delete;
        DecoyChild& operator=(const DecoyChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uint64_t decoy_address() const
        {
            return reinterpret_cast<std::uint64_t>(decoy_);
        }

        // Zero while nothing executed the decoy page: only the follower
        // instruction could set it.
        [[nodiscard]] std::uint64_t marker() const
        {
            return shared_ != nullptr ? shared_->marker : 0;
        }

        [[nodiscard]] std::uint64_t counter() const
        {
            return shared_ != nullptr ? shared_->counter : 0;
        }

        // True when the decoy page still holds exactly the bytes written to it,
        // so no window patched the target's own generated memory either.
        [[nodiscard]] bool decoy_intact() const
        {
            return decoy_ != nullptr && ::memcmp(decoy_, decoy_bytes_.data(), decoy_bytes_.size()) == 0;
        }

    private:
        void write_decoy()
        {
            // `syscall` ; `mov rax, <&marker>` ; `mov dword ptr [rax], 1` ; `ud2`.
            const std::uint64_t marker = reinterpret_cast<std::uint64_t>(&shared_->marker);
            decoy_bytes_[0]            = std::byte {0x0F};
            decoy_bytes_[1]            = std::byte {0x05};
            decoy_bytes_[2]            = std::byte {0x48};
            decoy_bytes_[3]            = std::byte {0xB8};
            for (std::size_t i = 0; i < 8; ++i)
            {
                decoy_bytes_[4 + i] = static_cast<std::byte>(static_cast<unsigned char>(marker >> (8 * i)));
            }
            decoy_bytes_[12] = std::byte {0xC7};
            decoy_bytes_[13] = std::byte {0x00};
            decoy_bytes_[14] = std::byte {0x01};
            decoy_bytes_[15] = std::byte {0x00};
            decoy_bytes_[16] = std::byte {0x00};
            decoy_bytes_[17] = std::byte {0x00};
            decoy_bytes_[18] = std::byte {0x0F};
            decoy_bytes_[19] = std::byte {0x0B};
            ::memcpy(decoy_, decoy_bytes_.data(), decoy_bytes_.size());
        }

        pid_t                     pid_ {-1};
        std::byte*                decoy_ {nullptr};
        Shared*                   shared_ {nullptr};
        std::array<std::byte, 20> decoy_bytes_ {};
    };

    // The two futex operations: the child blocks in a wait, the parent wakes it
    // again to prove it kept running.
    constexpr int kFutexWait = 0;
    constexpr int kFutexWake = 1;

    // A child whose leader is inside a blocking syscall when a window seizes it,
    // which is the reported Proton target's shape (its main thread sits in
    // `ioctl`/ntsync while slopkit attaches). The leader blocks in a futex wait
    // and must be making progress again afterwards. `workers` extra threads stay
    // in user mode, so a window that borrows one of them never touches the blocked
    // leader's own wait; `workers = 0` models a target whose only thread is
    // inside the call.
    class BlockedChild
    {
    public:
        struct Shared
        {
            std::uint64_t counter {};
            std::uint32_t word {};
        };

        // The counter value the child publishes right before it enters the wait.
        static constexpr std::uint64_t kInSyscall = 1;

        explicit BlockedChild(int workers = 4)
        {
            shared_ = static_cast<Shared*>(
                ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
            if (shared_ == MAP_FAILED || shared_ == nullptr)
            {
                shared_ = nullptr;
                return;
            }
            shared_->counter = 0;
            shared_->word    = 0;

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                // Pure user-mode spinners: they issue no syscall of their own,
                // so the window can borrow one of them without ever entering a
                // call the target started.
                for (int i = 0; i < workers; ++i)
                {
                    std::thread worker(
                        [shared = shared_]
                        {
                            volatile std::uint64_t spin = 0;
                            for (;;)
                            {
                                spin = spin + 1;
                            }
                        });
                    worker.detach();
                }

                // The parent waits for this value before it attaches, so the
                // block is the last thing the leader did.
                shared_->counter = kInSyscall;
                ::syscall(SYS_futex, &shared_->word, kFutexWait, 0, nullptr, nullptr, 0);
                for (;;)
                {
                    shared_->counter = shared_->counter + 1;
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~BlockedChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
            if (shared_ != nullptr)
            {
                ::munmap(shared_, sizeof(Shared));
            }
        }

        BlockedChild(const BlockedChild&)            = delete;
        BlockedChild& operator=(const BlockedChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uint64_t counter() const
        {
            return shared_ != nullptr ? shared_->counter : 0;
        }

        // Wakes the child's futex wait.
        void wake() const
        {
            if (shared_ != nullptr)
            {
                ::syscall(SYS_futex, &shared_->word, kFutexWake, 1, nullptr, nullptr, 0);
            }
        }

    private:
        pid_t   pid_ {-1};
        Shared* shared_ {nullptr};
    };

    // Set before the fork so the child's handler can write to the shared page it
    // belongs to; the parent's copy stays untouched.
    volatile std::uint64_t* g_signal_flag = nullptr;

    void mark_signal(int)
    {
        if (g_signal_flag != nullptr)
        {
            *g_signal_flag = 1;
        }
    }

    // A child that handles SIGUSR1 by flagging it in shared memory and then spins
    // on, so a signal delivered while a window has it stopped can be seen to have
    // reached the handler the target itself installed.
    class SignalledChild
    {
    public:
        struct Shared
        {
            std::uint64_t ready {};
            std::uint64_t handled {};
            std::uint64_t counter {};
        };

        SignalledChild()
        {
            shared_ = static_cast<Shared*>(
                ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
            if (shared_ == MAP_FAILED || shared_ == nullptr)
            {
                shared_ = nullptr;
                return;
            }
            shared_->handled = 0;
            shared_->counter = 0;

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                g_signal_flag = &shared_->handled;
                struct sigaction action {};
                action.sa_handler = mark_signal;
                ::sigemptyset(&action.sa_mask);
                ::sigaction(SIGUSR1, &action, nullptr);
                // The parent signals only once the handler is installed, so no
                // signal of its stream hits the default action.
                shared_->ready = 1;
                for (;;)
                {
                    shared_->counter = shared_->counter + 1;
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~SignalledChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
            if (shared_ != nullptr)
            {
                ::munmap(shared_, sizeof(Shared));
            }
        }

        SignalledChild(const SignalledChild&)            = delete;
        SignalledChild& operator=(const SignalledChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uint64_t handled() const
        {
            return shared_ != nullptr ? shared_->handled : 0;
        }

        [[nodiscard]] std::uint64_t counter() const
        {
            return shared_ != nullptr ? shared_->counter : 0;
        }

    private:
        pid_t   pid_ {-1};
        Shared* shared_ {nullptr};
    };

    // A target shaped like the reported Proton game: many threads, all running in
    // user mode, with signals arriving while a window borrows one of them. The
    // window may stop one thread at a time and must leave the rest running.
    class ManyThreadChild
    {
    public:
        struct Shared
        {
            std::uint64_t handled {};
            std::uint64_t counter {};
        };

        static constexpr int kWorkers = 8;

        ManyThreadChild()
        {
            shared_ = static_cast<Shared*>(
                ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
            if (shared_ == MAP_FAILED || shared_ == nullptr)
            {
                shared_ = nullptr;
                return;
            }
            shared_->handled = 0;
            shared_->counter = 0;

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                g_signal_flag = &shared_->handled;
                struct sigaction action {};
                action.sa_handler = mark_signal;
                ::sigemptyset(&action.sa_mask);
                ::sigaction(SIGUSR1, &action, nullptr);

                // Every thread counts, so any thread stopped for the length of a
                // window shows up in the shared counter.
                for (int i = 0; i < kWorkers; ++i)
                {
                    std::thread worker(
                        [shared = shared_]
                        {
                            for (;;)
                            {
                                shared->counter = shared->counter + 1;
                            }
                        });
                    worker.detach();
                }
                for (;;)
                {
                    shared_->counter = shared_->counter + 1;
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~ManyThreadChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
            if (shared_ != nullptr)
            {
                ::munmap(shared_, sizeof(Shared));
            }
        }

        ManyThreadChild(const ManyThreadChild&)            = delete;
        ManyThreadChild& operator=(const ManyThreadChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uint64_t handled() const
        {
            return shared_ != nullptr ? shared_->handled : 0;
        }

        [[nodiscard]] std::uint64_t counter() const
        {
            return shared_ != nullptr ? shared_->counter : 0;
        }

    private:
        pid_t   pid_ {-1};
        Shared* shared_ {nullptr};
    };

    // A target that takes an allocated address back for itself: on command it
    // replaces the page with a `MAP_FIXED` mapping of its own and fills it with a
    // canary, so a `dealloc` of an address the target no longer holds has to
    // refuse instead of unmapping the target's own memory.
    class ReclaimChild
    {
    public:
        static constexpr std::uint64_t kCanary = 0x51A11A11'5EED5EEDULL;

        struct Shared
        {
            std::uint64_t counter {};
            std::uint64_t address {};
            std::uint64_t size {};
            std::uint64_t wanted {};
            std::uint64_t done {};
            std::uint64_t canary {};
        };

        ReclaimChild()
        {
            shared_ = static_cast<Shared*>(
                ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
            if (shared_ == MAP_FAILED || shared_ == nullptr)
            {
                shared_ = nullptr;
                return;
            }
            shared_->counter = 0;
            shared_->address = 0;
            shared_->size    = 0;
            shared_->wanted  = 0;
            shared_->done    = 0;
            shared_->canary  = 0;

            const pid_t pid = ::fork();
            if (pid == 0)
            {
                for (;;)
                {
                    shared_->counter = shared_->counter + 1;
                    if (shared_->wanted != 0)
                    {
                        auto* taken = static_cast<std::uint64_t*>(::mmap(reinterpret_cast<void*>(shared_->address),
                                                                         static_cast<std::size_t>(shared_->size),
                                                                         PROT_READ | PROT_WRITE,
                                                                         MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                                                                         -1,
                                                                         0));
                        if (taken != MAP_FAILED && taken != nullptr)
                        {
                            *taken          = kCanary;
                            shared_->canary = *taken;
                        }
                        shared_->wanted = 0;
                        shared_->done   = 1;
                    }
                }
                ::_exit(0);
            }
            if (pid > 0)
            {
                pid_ = pid;
            }
        }

        ~ReclaimChild()
        {
            if (pid_ > 0)
            {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, nullptr, 0);
            }
            if (shared_ != nullptr)
            {
                ::munmap(shared_, sizeof(Shared));
            }
        }

        ReclaimChild(const ReclaimChild&)            = delete;
        ReclaimChild& operator=(const ReclaimChild&) = delete;

        [[nodiscard]] std::uint32_t pid() const
        {
            return static_cast<std::uint32_t>(pid_);
        }

        [[nodiscard]] std::uint64_t counter() const
        {
            return shared_ != nullptr ? shared_->counter : 0;
        }

        // Asks the child to take `address` back for `size` bytes and waits until
        // it did, so the address really is the child's own mapping afterwards.
        bool take_back(std::uint64_t address, std::uint64_t size)
        {
            if (shared_ == nullptr)
            {
                return false;
            }
            shared_->address = address;
            shared_->size    = size;
            shared_->done    = 0;
            shared_->wanted  = 1;
            for (int i = 0; i < 200; ++i)
            {
                if (shared_->done != 0)
                {
                    return shared_->canary == kCanary;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds {5});
            }
            return false;
        }

    private:
        pid_t   pid_ {-1};
        Shared* shared_ {nullptr};
    };

    // A stable fingerprint of the target's mappings, so a refused call can be
    // shown to have mapped and unmapped nothing at all. Only the geometry is
    // fingerprinted: permission flags and backing path do not change on their
    // own while the target sits in a wait.
    std::vector<std::string> map_signature(std::uint32_t pid)
    {
        std::vector<std::string> signature;
        for (const auto& region : slopkit::platform::read_maps(pid))
        {
            signature.push_back(std::to_string(region.start) + "-" + std::to_string(region.end) + " "
                                + (region.readable ? "r" : "-") + (region.writable ? "w" : "-")
                                + (region.executable ? "x" : "-") + " " + region.path);
        }
        return signature;
    }

    // True when no thread of `pid` sits in a ptrace stop any more.
    bool no_traced_stop(std::uint32_t pid)
    {
        for (const auto& thread : slopkit::platform::read_threads(pid))
        {
            if (slopkit::platform::read_thread_state(pid, thread.tid) == 't')
            {
                return false;
            }
        }
        return true;
    }

    template<typename Child>
    bool await_counter(const Child& child, std::uint64_t want)
    {
        for (int i = 0; i < 200; ++i)
        {
            if (child.counter() >= want)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds {5});
        }
        return false;
    }
} // namespace

TEST_CASE("allocation completes while the target is inside a syscall", "[linux_proc][alloc]")
{
    BlockedChild child;
    REQUIRE(child.pid() != 0);

    // Attach only once the child reached the wait, and give it a moment to be
    // inside the kernel, so the window really meets a thread mid-syscall. The
    // leader is in the futex; the window has to borrow one of the user-mode
    // workers instead of stealing the leader's wait.
    REQUIRE(await_counter(child, BlockedChild::kInSyscall));
    std::this_thread::sleep_for(std::chrono::milliseconds {50});

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    const auto allocated = session->allocate_memory(page, 0);
    REQUIRE(allocated.has_value());
    CHECK(*allocated != 0);
    CHECK((*allocated % page) == 0);
    CHECK(mapped_rwx(child.pid(), *allocated, page));

    // The interrupted syscall was handed back: the child runs on once woken.
    child.wake();
    wait_for_progress(child);

    REQUIRE(session->free_memory(*allocated).has_value());
    wait_for_progress(child);
}

TEST_CASE("the window borrows one thread and leaves the others running", "[linux_proc][alloc]")
{
    ManyThreadChild child;
    REQUIRE(child.pid() != 0);
    REQUIRE(await_counter(child, 1));

    // More than one thread is what makes the single-thread window observable.
    REQUIRE(slopkit::platform::read_threads(child.pid()).size() > 1);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    // Samples the thread states for as long as the windows run: at most the one
    // borrowed donor may ever sit in a traced stop.
    std::atomic<bool>        monitoring {true};
    std::atomic<std::size_t> most_stopped {0};
    std::thread              monitor(
        [&]
        {
            while (monitoring.load())
            {
                std::size_t stopped = 0;
                for (const auto& thread : slopkit::platform::read_threads(child.pid()))
                {
                    if (slopkit::platform::read_thread_state(child.pid(), thread.tid) == 't')
                    {
                        ++stopped;
                    }
                }
                std::size_t previous = most_stopped.load();
                while (stopped > previous && !most_stopped.compare_exchange_weak(previous, stopped))
                {
                }
            }
        });

    // A signal storm arrives while the windows borrow threads; each one has to
    // reach the target's own handler rather than being swallowed.
    std::atomic<bool> signalling {true};
    std::thread       signaller(
        [&]
        {
            while (signalling.load())
            {
                ::kill(child.pid(), SIGUSR1);
                std::this_thread::sleep_for(std::chrono::milliseconds {5});
            }
        });

    bool every_call_succeeded = true;
    for (int round = 0; round < 10 && every_call_succeeded; ++round)
    {
        const auto allocated = session->allocate_memory(page, 0);
        if (!allocated)
        {
            every_call_succeeded = false;
            break;
        }
        if (*allocated % page != 0 || !mapped_rwx(child.pid(), *allocated, page))
        {
            every_call_succeeded = false;
            break;
        }
        if (!session->free_memory(*allocated).has_value())
        {
            every_call_succeeded = false;
            break;
        }
    }

    signalling = false;
    signaller.join();
    monitoring = false;
    monitor.join();

    REQUIRE(every_call_succeeded);
    CHECK(most_stopped.load() <= 1);
    CHECK(no_traced_stop(child.pid()));
    CHECK(child.handled() != 0);
    wait_for_progress(child);
}

TEST_CASE("a target whose only thread is inside a syscall is refused", "[linux_proc][alloc]")
{
    // No worker: the leader is the target's only thread and it is parked in a
    // futex wait, so there is no thread the window may borrow.
    BlockedChild child(0);
    REQUIRE(child.pid() != 0);
    REQUIRE(await_counter(child, BlockedChild::kInSyscall));
    std::this_thread::sleep_for(std::chrono::milliseconds {50});

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    const auto before  = map_signature(child.pid());
    const auto refused = session->allocate_memory(0x1000, 0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == slopkit::process::AccessError::permission_denied);

    // The refusal leaves the target untouched: no thread in a traced stop, no
    // mapping created or removed, and the in-flight wait still delivers its real
    // result once woken.
    CHECK(no_traced_stop(child.pid()));
    CHECK(map_signature(child.pid()) == before);

    // The session is not wedged by the refusal either: the next attempt is still
    // made, and while the leader is still blocked it is refused for the same
    // truthful reason.
    const auto again = session->allocate_memory(0x1000, 0);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == slopkit::process::AccessError::permission_denied);
    CHECK(no_traced_stop(child.pid()));

    child.wake();
    wait_for_progress(child);

    // With the leader out of its wait the target has a thread to borrow, so the
    // very next call succeeds: a refusal is not a broken session.
    const auto later = session->allocate_memory(0x1000, 0);
    REQUIRE(later.has_value());
    CHECK(*later != 0);
    CHECK(no_traced_stop(child.pid()));
    REQUIRE(session->free_memory(*later).has_value());
    wait_for_progress(child);
}

TEST_CASE("a signal storm that keeps the target busy is survived harmlessly", "[linux_proc][alloc]")
{
    ManyThreadChild child;
    REQUIRE(child.pid() != 0);
    REQUIRE(await_counter(child, 1));

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page   = 0x1000;
    const auto            before = map_signature(child.pid());

    // Every thread gets a signal as fast as the test can send them, so the window
    // meets stops it did not ask for on almost every resume.
    std::atomic<bool> storming {true};
    std::thread       storm(
        [&]
        {
            while (storming.load())
            {
                for (const auto& thread : slopkit::platform::read_threads(child.pid()))
                {
                    ::syscall(SYS_tkill, static_cast<pid_t>(thread.tid), SIGUSR1);
                }
                std::this_thread::sleep_for(std::chrono::microseconds {200});
            }
        });

    bool ok = true;
    for (int round = 0; round < 5 && ok; ++round)
    {
        const auto allocated = session->allocate_memory(page, 0);
        if (!allocated)
        {
            // A refused call is the intended trade: it is checked below to have
            // left nothing behind.
            break;
        }
        if (!session->free_memory(*allocated).has_value())
        {
            ok = false;
        }
    }

    storming = false;
    storm.join();

    // Whether every call went through or the storm forced a refusal, the target
    // is exactly where it started and no thread is left stopped.
    REQUIRE(ok);
    CHECK(no_traced_stop(child.pid()));
    CHECK(map_signature(child.pid()) == before);
    CHECK(child.handled() != 0);
    wait_for_progress(child);
}

TEST_CASE("a signal the target receives during the window reaches its handler", "[linux_proc][alloc]")
{
    SignalledChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    // The child installs its handler before it starts counting, so reaching the
    // first count means a SIGUSR1 is now handled rather than fatal.
    REQUIRE(await_counter(child, 1));

    // A few signals land while the windows seize the target: each one has to be
    // handed to the target's own handler rather than cancelled (or swallowed) by
    // the hand-back. A sustained stream is not what the window is asked to
    // survive — a target that never lets the call run is refused instead.
    std::atomic<bool> signalling {true};
    std::thread       signaller(
        [&]
        {
            for (int i = 0; i < 5 && signalling.load(); ++i)
            {
                ::kill(child.pid(), SIGUSR1);
                std::this_thread::sleep_for(std::chrono::milliseconds {10});
            }
        });

    for (int round = 0; round < 4; ++round)
    {
        const auto allocated = session->allocate_memory(page, 0);
        REQUIRE(allocated.has_value());
        CHECK(*allocated != 0);
        REQUIRE(session->free_memory(*allocated).has_value());
    }

    signalling = false;
    signaller.join();

    bool handler_ran = false;
    for (int waited = 0; waited < 200 && !handler_ran; ++waited)
    {
        handler_ran = child.handled() != 0;
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    CHECK(handler_ran);

    // No thread is left in a traced stop and the target keeps running by itself.
    CHECK(no_traced_stop(child.pid()));
    wait_for_progress(child);
}

TEST_CASE("the gadget scan never uses the target's own anonymous code", "[linux_proc][alloc]")
{
    DecoyChild child;
    REQUIRE(child.pid() != 0);
    REQUIRE(child.decoy_address() != 0);
    REQUIRE(mapped(child.pid(), child.decoy_address(), DecoyChild::kDecoySize));

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    // The decoy is the child's lowest executable mapping; a scan that only
    // filtered on `executable` would take its `0F 05` pair as the gadget.
    auto allocated = session->allocate_memory(page, 0);
    REQUIRE(allocated.has_value());
    CHECK(*allocated != 0);
    CHECK((*allocated % page) == 0);
    CHECK(mapped_rwx(child.pid(), *allocated, page));

    // Nothing of the decoy page ran and nothing patched it.
    CHECK(child.marker() == 0);
    CHECK(child.decoy_intact());
    wait_for_progress(child);

    REQUIRE(session->free_memory(*allocated).has_value());
    wait_for_progress(child);
}

TEST_CASE("linux-proc maps and unmaps target memory through the ABI", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    // A hint-less mapping lands page-aligned and read/write/execute-able.
    auto allocated = session->allocate_memory(page, 0);
    REQUIRE(allocated.has_value());
    const std::uint64_t address = *allocated;
    CHECK(address != 0);
    CHECK((address % page) == 0);
    CHECK(mapped_rwx(child.pid(), address, page));

    // The mapping is real memory: bytes written through the plugin read back.
    const std::vector<std::byte>   payload {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};
    slopkit::process::AccessMethod method  = slopkit::process::AccessMethod::none;
    auto                           written = session->write(address, payload, method);
    REQUIRE(written.has_value());
    CHECK(*written == payload.size());

    auto read = session->read(address, 4, method);
    REQUIRE(read.has_value());
    CHECK(*read == payload);

    // The target itself keeps running across the whole sequence.
    wait_for_progress(child);

    // Freeing removes exactly that mapping.
    auto freed = session->free_memory(address);
    REQUIRE(freed.has_value());
    CHECK_FALSE(mapped(child.pid(), address, page));
    wait_for_progress(child);

    // Freeing it again reports not-found; a foreign address (a module base) is
    // never unmapped.
    auto twice = session->free_memory(address);
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error() == slopkit::process::AccessError::not_found);

    auto modules = session->modules();
    REQUIRE(modules.has_value());
    REQUIRE(!modules->empty());
    auto foreign = session->free_memory(modules->front().base);
    REQUIRE_FALSE(foreign.has_value());
    CHECK(foreign.error() == slopkit::process::AccessError::not_found);
    CHECK(mapped(child.pid(), modules->front().base, page));

    // Zero size and a refused mapping are reported, never a bogus address.
    auto zero = session->allocate_memory(0, 0);
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error() == slopkit::process::AccessError::invalid_argument);
}

TEST_CASE("dealloc refuses an address the target has taken back", "[linux_proc][alloc]")
{
    ReclaimChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    const auto allocated = session->allocate_memory(page, 0);
    REQUIRE(allocated.has_value());

    // The target replaces the page with a mapping of its own and fills it in.
    REQUIRE(child.take_back(*allocated, page));

    // The target's own bytes now live where the window's mapping was.
    slopkit::process::AccessMethod method = slopkit::process::AccessMethod::none;
    std::uint64_t                  value  = 0;
    const auto                     before = session->read(*allocated, 8, method);
    REQUIRE(before.has_value());
    std::memcpy(&value, before->data(), sizeof(value));
    CHECK(value == ReclaimChild::kCanary);

    // The free is refused with a reason instead of unmapping the target's page.
    const auto refused = session->free_memory(*allocated);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == slopkit::process::AccessError::not_found);

    // The target's mapping is intact, still its own and still mapped.
    const auto after = session->read(*allocated, 8, method);
    REQUIRE(after.has_value());
    std::memcpy(&value, after->data(), sizeof(value));
    CHECK(value == ReclaimChild::kCanary);
    CHECK(mapped(child.pid(), *allocated, page));

    // The refusal is not a stale success: the next attempt refuses the same way.
    const auto again = session->free_memory(*allocated);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == slopkit::process::AccessError::not_found);

    wait_for_progress(child);
}

TEST_CASE("linux-proc honours a best-effort allocation hint", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());

    // Take a mapping, free it, then ask for the very same address: it is free
    // again, so the hint is honoured exactly.
    auto first = session->allocate_memory(0x2000, 0);
    REQUIRE(first.has_value());
    REQUIRE(session->free_memory(*first).has_value());

    auto hinted = session->allocate_memory(0x2000, *first);
    REQUIRE(hinted.has_value());
    CHECK(*hinted == *first);
    REQUIRE(session->free_memory(*hinted).has_value());
}

TEST_CASE("linux-proc maps near a hint that is already taken", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    constexpr std::size_t page = 0x1000;

    // Hold a mapping so the page at its base is taken. Hinting it must land in
    // the closest free page instead of giving up and mapping anywhere.
    auto taken = session->allocate_memory(page, 0);
    REQUIRE(taken.has_value());
    REQUIRE(mapped(child.pid(), *taken, page));

    // Fault the hinted page in and leave a marker in it. The nearest free page
    // abuts this one, so the kernel usually merges the new mapping into a
    // single region starting at either base — which is why the checks here are
    // coverage-based. Reading the marker back after the hinted allocation
    // proves the hinted page's contents survived, without relying on a VMA
    // boundary.
    const std::vector<std::byte>   payload {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}};
    slopkit::process::AccessMethod method = slopkit::process::AccessMethod::none;
    auto                           marker = session->write(*taken, payload, method);
    REQUIRE(marker.has_value());
    CHECK(*marker == payload.size());

    auto near = session->allocate_memory(page, *taken);
    REQUIRE(near.has_value());
    const std::uint64_t address = *near;
    CHECK((address % page) == 0);
    CHECK(address != *taken);
    CHECK(mapped_rwx(child.pid(), address, page));

    // Close enough for the ±2 GB a near jump reaches (in practice a few pages).
    const std::uint64_t distance = address > *taken ? address - *taken : *taken - address;
    CHECK(distance <= 0x80000000ull);

    // The hinted region itself is untouched: its marker still reads back.
    CHECK(mapped(child.pid(), *taken, page));
    auto read = session->read(*taken, payload.size(), method);
    REQUIRE(read.has_value());
    CHECK(*read == payload);

    REQUIRE(session->free_memory(address).has_value());
    REQUIRE(session->free_memory(*taken).has_value());
}

TEST_CASE("a debug session and an allocation never overlap", "[linux_proc][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("linux-proc");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());

    auto* vtable = session->vtable();
    void* handle = session->handle();
    REQUIRE(vtable != nullptr);
    REQUIRE(vtable->debug_attach != nullptr);

    std::uint32_t leader = 0;
    REQUIRE(vtable->debug_attach(handle, &leader).code == SLOPKIT_OK);

    // The debugger owns the thread group, so allocation is refused.
    auto refused = session->allocate_memory(0x1000, 0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == slopkit::process::AccessError::permission_denied);

    REQUIRE(vtable->debug_detach(handle).code == SLOPKIT_OK);

    // Once the debugger lets go, allocation works again.
    auto allocated = session->allocate_memory(0x1000, 0);
    REQUIRE(allocated.has_value());
    REQUIRE(session->free_memory(*allocated).has_value());
}

TEST_CASE("wine-proton also maps and unmaps target memory", "[wine_proton][alloc]")
{
    AllocChild child;
    REQUIRE(child.pid() != 0);

    slopkit::plugin::PluginHost host;
    host.discover({SLOPKIT_PLUGIN_DIR});

    auto* plugin = host.find("wine-proton");
    REQUIRE(plugin != nullptr);

    auto session = plugin->open_session(child.pid());
    REQUIRE(session.has_value());
    REQUIRE(session->supports_allocation());

    auto allocated = session->allocate_memory(0x1000, 0);
    REQUIRE(allocated.has_value());
    CHECK((*allocated % 0x1000) == 0);
    REQUIRE(session->free_memory(*allocated).has_value());
}
