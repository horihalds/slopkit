#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scan/results.hpp"
#include "scan/source.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::scan
{
    struct ScanConfig
    {
        ScanType     type {ScanType::exact_value};
        ValueType    value_type {ValueType::int32};
        ScanValue    value {std::int64_t {0}};
        ScanValue    value_upper {std::int64_t {0}};
        bool         hex {false};
        RegionFilter filter;
    };

    inline constexpr std::size_t kDisplayPage = 256;

    struct ScanSnapshot
    {
        ScanState                          state {ScanState::idle};
        float                              progress {0.0f};
        std::size_t                        scanned_bytes {};
        std::size_t                        total_bytes {};
        std::size_t                        hit_count {};
        ScanResults                        page;
        std::shared_ptr<const ScanResults> result_hits;
        std::string                        message;
    };

    class ScanEngine
    {
    public:
        ScanEngine();
        ~ScanEngine();

        ScanEngine(const ScanEngine&)            = delete;
        ScanEngine& operator=(const ScanEngine&) = delete;

        void first_scan(ScanConfig config, MemorySource source);

        void next_scan(ScanConfig config);

        void undo();

        void cancel();

        void reset();

        [[nodiscard]] ScanSnapshot snapshot() const;
        [[nodiscard]] bool         is_running() const noexcept;
        [[nodiscard]] ScanConfig   config() const;

        [[nodiscard]] std::size_t result_count() const;

        [[nodiscard]] bool has_results() const;

        [[nodiscard]] bool can_undo() const;

        void                      set_max_threads(std::size_t threads) noexcept;
        [[nodiscard]] std::size_t max_threads() const noexcept;

    private:
        using ResultSetPtr = std::shared_ptr<const ScanResults>;

        static constexpr std::size_t kHistoryDepth   = 8;
        static constexpr std::size_t kMaxScanThreads = 8;

        void run_first(ScanConfig config, MemorySource source, const std::stop_token& token);
        void run_next(ScanConfig config, ResultSetPtr previous, MemorySource source, const std::stop_token& token);

        void publish_running_locked(std::size_t scanned, std::size_t total, std::size_t count);
        void publish_results_locked(ScanState state, std::string message);
        void finish_success(std::shared_ptr<ScanResults> results, bool reset_history);
        void finish_worker() noexcept;

        mutable std::mutex        mutex_;
        ScanSnapshot              snapshot_;
        ResultSetPtr              results_;
        std::vector<ResultSetPtr> history_;
        ScanConfig                config_;
        MemorySource              source_;
        std::atomic<bool>         cancel_requested_ {false};
        std::atomic<std::size_t>  max_threads_ {0};
        std::jthread              worker_;
    };

}
