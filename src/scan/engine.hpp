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

    struct ScanSnapshot
    {
        ScanState            state {ScanState::idle};
        float                progress {0.0f};
        std::size_t          scanned_bytes {};
        std::size_t          total_bytes {};
        std::size_t          hit_count {};
        bool                 truncated {};
        std::vector<ScanHit> hits; // Capped display page.
        std::string          message;
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

        [[nodiscard]] ScanSnapshot snapshot() const;
        [[nodiscard]] bool         is_running() const noexcept;
        [[nodiscard]] ScanConfig   config() const;

        // The number of stored hits in the current result set.
        [[nodiscard]] std::size_t result_count() const;

        // True once a scan has produced a result set, even an empty one.
        [[nodiscard]] bool has_results() const;

        // Lowers or raises the stored-hit cap; takes effect on the next scan.
        void                      set_max_stored_hits(std::size_t maximum) noexcept;
        [[nodiscard]] std::size_t max_stored_hits() const noexcept;

    private:
        // The full result set of a scan; `count` may exceed `hits.size()` when
        // the stored hits were capped.
        struct ResultSet
        {
            std::vector<ScanHit> hits;
            std::size_t          count {};
            bool                 truncated {};
        };

        using ResultSetPtr = std::shared_ptr<const ResultSet>;

        static constexpr std::size_t kMaxStoredHits = 1'000'000;
        static constexpr std::size_t kDisplayPage   = 256;
        static constexpr std::size_t kHistoryDepth  = 8;

        void run_first(ScanConfig config, MemorySource source, const std::stop_token& token);
        void run_next(ScanConfig config, ResultSetPtr previous, MemorySource source, const std::stop_token& token);

        void publish_running_locked(const std::vector<ScanHit>& hits,
                                    std::size_t                 scanned,
                                    std::size_t                 total,
                                    std::size_t                 count,
                                    bool                        truncated);
        void publish_results_locked(ScanState state, std::string message);
        void finish_success(std::shared_ptr<std::vector<ScanHit>> hits,
                            std::size_t                           count,
                            bool                                  truncated,
                            bool                                  reset_history);
        void finish_worker() noexcept;

        mutable std::mutex        mutex_;
        ScanSnapshot              snapshot_;
        ResultSetPtr              results_;
        std::vector<ResultSetPtr> history_;
        ScanConfig                config_;
        MemorySource              source_;
        std::atomic<bool>         cancel_requested_ {false};
        std::atomic<std::size_t>  max_stored_hits_ {kMaxStoredHits};
        std::jthread              worker_;
    };

} // namespace slopkit::scan
