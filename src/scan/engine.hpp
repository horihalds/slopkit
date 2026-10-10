#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scan/source.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::scan
{

    // Everything a scan needs: the comparison, the value type, the value(s) and
    // the region filter.
    struct ScanConfig
    {
        ScanType     type {ScanType::exact_value};
        ValueType    value_type {ValueType::int32};
        ScanValue    value {std::int64_t {0}};
        ScanValue    value_upper {std::int64_t {0}};
        bool         hex {false};
        RegionFilter filter;
    };

    // One matched address with its current and previous bytes.
    struct ScanHit
    {
        std::uint64_t          address {};
        std::vector<std::byte> value;
        std::vector<std::byte> previous;
    };

    // The rows the result list shows at once: the top page of the static-first
    // ordering over the whole stored result set.
    inline constexpr std::size_t kDisplayPage = 256;

    struct ScanSnapshot
    {
        ScanState                                   state {ScanState::idle};
        float                                       progress {0.0f};
        std::size_t                                 scanned_bytes {};
        std::size_t                                 total_bytes {};
        std::size_t                                 hit_count {};
        // The top `kDisplayPage` rows of a finished result set; empty while a
        // scan runs.
        std::vector<ScanHit>                        hits;
        // The whole stored result set once a scan finished (null while running or
        // before any result). Shared with the engine, so copies stay cheap and the
        // pointer doubles as the change signal for the result list.
        std::shared_ptr<const std::vector<ScanHit>> result_hits;
        std::string                                 message;
    };

    // Finds and refines values in an address space on a worker thread. The UI
    // polls snapshot() each frame and never blocks; the engine owns its worker
    // and joins it in the destructor.
    class ScanEngine
    {
    public:
        ScanEngine();
        ~ScanEngine();

        ScanEngine(const ScanEngine&)            = delete;
        ScanEngine& operator=(const ScanEngine&) = delete;

        // Starts a brand-new scan over `source` and clears the undo history.
        void first_scan(ScanConfig config, MemorySource source);

        // Refines the current result set; needs a previous scan.
        void next_scan(ScanConfig config);

        // Restores the result set from before the last refinement.
        void undo();

        // Requests the running scan to stop; the results are left intact.
        void cancel();

        // Stops a running scan and returns the engine to its constructed state:
        // no result set, no undo history, no message, progress 0.
        void reset();

        [[nodiscard]] ScanSnapshot snapshot() const;
        [[nodiscard]] bool         is_running() const noexcept;
        [[nodiscard]] ScanConfig   config() const;

        // The number of stored hits in the current result set.
        [[nodiscard]] std::size_t result_count() const;

        // True once a scan has produced a result set, even an empty one.
        [[nodiscard]] bool has_results() const;

        // True while the undo history holds a refinement to restore, i.e. a Next
        // Scan has been applied that has not been undone yet.
        [[nodiscard]] bool can_undo() const;

        // Overrides the first-scan worker count: 0 means automatic, 1 forces the
        // sequential path. Used by the tests and the throughput benchmark.
        void                      set_max_threads(std::size_t threads) noexcept;
        [[nodiscard]] std::size_t max_threads() const noexcept;

    private:
        // The whole result set of a scan, in address order. Shared with the UI
        // through the snapshot, so copies stay cheap.
        using ResultSetPtr = std::shared_ptr<const std::vector<ScanHit>>;

        static constexpr std::size_t kHistoryDepth   = 8;
        static constexpr std::size_t kMaxScanThreads = 8;

        void run_first(ScanConfig config, MemorySource source, const std::stop_token& token);
        void run_next(ScanConfig config, ResultSetPtr previous, MemorySource source, const std::stop_token& token);

        // Publishes progress only: a running scan exposes no rows at all.
        void publish_running_locked(std::size_t scanned, std::size_t total, std::size_t count);
        void publish_results_locked(ScanState state, std::string message);
        void finish_success(std::shared_ptr<std::vector<ScanHit>> hits, bool reset_history);
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

} // namespace slopkit::scan
