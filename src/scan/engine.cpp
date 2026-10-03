#include "scan/engine.hpp"

#include <algorithm>
#include <expected>
#include <utility>

#include "scan/matcher.hpp"

namespace slopkit::scan
{

    namespace
    {
        // Region spans are read in bounded chunks so progress advances and
        // cancellation is noticed promptly.
        constexpr std::size_t kChunkBytes = 1u << 22;

        // A first scan is partitioned into ~32 MB shards that workers pull in
        // parallel; shards are independent, so this is only about load balance.
        constexpr std::size_t kShardBytes = 1u << 25;

        // How often the worker publishes progress, to limit lock churn.
        constexpr std::size_t kPublishEveryCandidate = 1024;

        std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
        {
            if (alignment <= 1)
            {
                return value;
            }
            const std::uint64_t remainder = value % alignment;
            return remainder == 0 ? value : value + (alignment - remainder);
        }

        struct Span
        {
            std::uint64_t begin {};
            std::uint64_t end {};
        };

        // Applies the region filter to a region list and aligns the results.
        std::vector<Span> build_spans(const std::vector<process::RegionInfo>& regions, const RegionFilter& filter)
        {
            const std::uint64_t alignment      = filter.alignment == 0 ? 1 : filter.alignment;
            const bool          use_attributes = filter.writable || filter.executable || filter.copy_on_write;

            std::vector<Span> spans;
            for (const auto& region : regions)
            {
                if (!region.readable || region.end <= region.start)
                {
                    continue;
                }
                if (use_attributes)
                {
                    const bool matches = (filter.writable && region.writable)
                                      || (filter.executable && region.executable)
                                      || (filter.copy_on_write && is_copy_on_write(region));
                    if (!matches)
                    {
                        continue;
                    }
                }

                std::uint64_t begin = region.start;
                std::uint64_t end   = region.end;
                if (filter.start != 0 && begin < filter.start)
                {
                    begin = filter.start;
                }
                if (filter.stop != 0 && end > filter.stop)
                {
                    end = filter.stop;
                }
                if (end <= begin)
                {
                    continue;
                }
                begin = align_up(begin, alignment);
                if (end <= begin)
                {
                    continue;
                }
                spans.push_back(Span {begin, end});
            }
            return spans;
        }

        std::size_t total_bytes(const std::vector<Span>& spans)
        {
            std::size_t total = 0;
            for (const auto& span : spans)
            {
                total += static_cast<std::size_t>(span.end - span.begin);
            }
            return total;
        }

        // The first scan is split into shards that workers pull dynamically. A
        // shard boundary is aligned so the candidate grid is exactly the one the
        // sequential scan would have walked; `limit` is the end of the containing
        // span, beyond which a read may not go.
        struct Shard
        {
            std::uint64_t begin {};
            std::uint64_t end {};
            std::uint64_t limit {};
        };

        std::vector<Shard> build_shards(const std::vector<Span>& spans, std::uint64_t alignment)
        {
            std::vector<Shard> shards;
            for (const auto& span : spans)
            {
                std::uint64_t cursor = span.begin;
                while (cursor < span.end)
                {
                    std::uint64_t end = std::min<std::uint64_t>(cursor + kShardBytes, span.end);
                    if (alignment > 1)
                    {
                        end = std::min(align_up(end, alignment), span.end);
                    }
                    if (end <= cursor)
                    {
                        end = span.end;
                    }
                    shards.push_back(Shard {cursor, end, span.end});
                    cursor = end;
                }
            }
            return shards;
        }

        // One worker's hits for one shard, with a release flag the progress
        // publisher uses to skip shards that are still being written.
        struct ShardHits
        {
            std::vector<ScanHit> hits;
            std::atomic<bool>    done {false};
        };

        struct ScannedShard
        {
            std::vector<ScanHit> hits;
            bool                 cancelled {};
        };

        // Walks one shard in chunks, reading one extra window across the chunk
        // and shard boundaries so windows that straddle them are not skipped.
        // Candidates only start inside the chunk, so `scanned` accounting is
        // unchanged. Dynamic slots are claimed from `stored` so the total number
        // of kept hits never exceeds `cap`.
        ScannedShard scan_shard_range(const Shard&              shard,
                                      const Matcher&            matcher,
                                      const MemorySource&       source,
                                      std::uint64_t             alignment,
                                      std::size_t               size,
                                      std::size_t               cap,
                                      std::vector<std::byte>&   buffer,
                                      std::atomic<std::size_t>& stored,
                                      std::atomic<std::size_t>& scanned,
                                      std::atomic<std::size_t>& found,
                                      const std::stop_token&    token,
                                      const std::atomic<bool>&  cancel_requested)
        {
            ScannedShard      result;
            const std::size_t overlap = size > alignment ? size - static_cast<std::size_t>(alignment) : 0;

            std::uint64_t cursor = shard.begin;
            while (cursor < shard.end)
            {
                if (token.stop_requested() || cancel_requested.load())
                {
                    result.cancelled = true;
                    return result;
                }

                std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(shard.end - cursor, kChunkBytes));
                if (alignment > 1)
                {
                    chunk -= chunk % static_cast<std::size_t>(alignment);
                }
                if (chunk == 0)
                {
                    chunk = static_cast<std::size_t>(shard.end - cursor);
                }

                const std::size_t available = static_cast<std::size_t>(shard.limit - cursor);
                const std::size_t read_size = std::min(chunk + overlap, available);

                std::span<const std::byte>                                  bytes;
                std::expected<std::vector<std::byte>, process::AccessError> owned;
                if (source.read_into)
                {
                    if (buffer.size() < read_size)
                    {
                        buffer.resize(read_size);
                    }
                    std::expected<std::size_t, process::AccessError> count;
                    try
                    {
                        count = source.read_into(cursor, std::span<std::byte>(buffer.data(), read_size));
                    }
                    catch (...)
                    {
                        count = std::unexpected(process::AccessError::internal);
                    }
                    if (count && *count > 0)
                    {
                        bytes = std::span<const std::byte>(buffer.data(), *count);
                    }
                }
                else
                {
                    try
                    {
                        owned = source.read(cursor, read_size);
                    }
                    catch (...)
                    {
                        owned = std::unexpected(process::AccessError::internal);
                    }
                    if (owned && !owned->empty())
                    {
                        bytes = std::span<const std::byte>(*owned);
                    }
                }

                if (!bytes.empty())
                {
                    const auto record = [&](std::uint64_t address, const std::byte* first)
                    {
                        found.fetch_add(1, std::memory_order_relaxed);
                        if (stored.fetch_add(1, std::memory_order_relaxed) < cap)
                        {
                            result.hits.push_back(ScanHit {address, std::vector<std::byte>(first, first + size), {}});
                        }
                    };

                    if (matcher.matches_all())
                    {
                        for (std::size_t offset = 0; offset + size <= bytes.size();
                             offset += static_cast<std::size_t>(alignment))
                        {
                            record(cursor + offset, bytes.data() + offset);
                        }
                    }
                    else
                    {
                        for (std::size_t offset = 0; offset < bytes.size();)
                        {
                            const std::size_t hit =
                                matcher.find(bytes, offset, bytes.size(), static_cast<std::size_t>(alignment));
                            if (hit == bytes.size())
                            {
                                break;
                            }
                            record(cursor + hit, bytes.data() + hit);
                            offset = hit + static_cast<std::size_t>(alignment);
                        }
                    }
                }

                scanned.fetch_add(chunk, std::memory_order_relaxed);
                cursor += chunk;
            }
            return result;
        }

        // Refreshes the running snapshot from the finished shards only, gathering
        // at most `page` hits in address order.
        void merge_page_locked(ScanSnapshot&                 snapshot,
                               const std::vector<ShardHits>& shards,
                               std::size_t                   page,
                               std::size_t                   scanned,
                               std::size_t                   total,
                               std::size_t                   count,
                               bool                          truncated)
        {
            snapshot.state = ScanState::running;
            snapshot.progress =
                total == 0
                    ? 1.0f
                    : std::min(1.0f, static_cast<float>(static_cast<double>(scanned) / static_cast<double>(total)));
            snapshot.scanned_bytes = scanned;
            snapshot.total_bytes   = total;
            snapshot.hit_count     = count;
            snapshot.truncated     = truncated;
            snapshot.hits.clear();
            snapshot.result_hits.reset();
            for (const auto& shard : shards)
            {
                if (!shard.done.load(std::memory_order_acquire))
                {
                    continue;
                }
                for (const auto& hit : shard.hits)
                {
                    if (snapshot.hits.size() >= page)
                    {
                        return;
                    }
                    snapshot.hits.push_back(hit);
                }
            }
        }
    } // namespace

    ScanEngine::ScanEngine() = default;

    ScanEngine::~ScanEngine()
    {
        finish_worker();
    }

    void ScanEngine::finish_worker() noexcept
    {
        if (!worker_.joinable())
        {
            return;
        }
        cancel_requested_.store(true);
        worker_.request_stop();
        worker_.join();
    }

    void ScanEngine::first_scan(ScanConfig config, MemorySource source)
    {
        finish_worker();

        MemorySource thread_source = source;
        {
            const std::lock_guard lock(mutex_);
            config_         = config;
            source_         = source;
            snapshot_       = ScanSnapshot {};
            snapshot_.state = ScanState::running;
        }

        cancel_requested_.store(false);
        worker_ = std::jthread(
            [this, config, thread_source = std::move(thread_source)](std::stop_token token) mutable
            {
                run_first(config, std::move(thread_source), token);
            });
    }

    void ScanEngine::next_scan(ScanConfig config)
    {
        finish_worker();

        ResultSetPtr previous;
        MemorySource source;
        {
            const std::lock_guard lock(mutex_);
            if (!results_)
            {
                snapshot_.state   = ScanState::failed;
                snapshot_.message = "Run a first scan before refining.";
                return;
            }
            previous        = results_;
            source          = source_;
            config_         = config;
            snapshot_.state = ScanState::running;
            snapshot_.message.clear();
        }

        cancel_requested_.store(false);
        worker_ = std::jthread(
            [this, config, previous, source = std::move(source)](std::stop_token token) mutable
            {
                run_next(config, previous, std::move(source), token);
            });
    }

    void ScanEngine::undo()
    {
        const std::lock_guard lock(mutex_);
        if (history_.empty())
        {
            snapshot_.message = "Nothing to undo.";
            return;
        }
        results_ = history_.back();
        history_.pop_back();
        publish_results_locked(ScanState::done, "Undid the last scan.");
    }

    void ScanEngine::cancel()
    {
        {
            const std::lock_guard lock(mutex_);
            if (snapshot_.state != ScanState::running)
            {
                return;
            }
        }
        cancel_requested_.store(true);
        worker_.request_stop();
    }

    void ScanEngine::reset()
    {
        finish_worker();

        const std::lock_guard lock(mutex_);
        snapshot_ = ScanSnapshot {};
        results_.reset();
        history_.clear();
        config_ = ScanConfig {};
        source_ = MemorySource {};
    }

    ScanSnapshot ScanEngine::snapshot() const
    {
        const std::lock_guard lock(mutex_);
        return snapshot_;
    }

    bool ScanEngine::is_running() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return snapshot_.state == ScanState::running;
    }

    ScanConfig ScanEngine::config() const
    {
        const std::lock_guard lock(mutex_);
        return config_;
    }

    std::size_t ScanEngine::result_count() const
    {
        const std::lock_guard lock(mutex_);
        return results_ ? results_->hits.size() : 0;
    }

    bool ScanEngine::has_results() const
    {
        const std::lock_guard lock(mutex_);
        return results_ != nullptr;
    }

    void ScanEngine::set_max_stored_hits(std::size_t maximum) noexcept
    {
        max_stored_hits_.store(maximum == 0 ? 1 : maximum);
    }

    std::size_t ScanEngine::max_stored_hits() const noexcept
    {
        return max_stored_hits_.load();
    }

    void ScanEngine::set_max_threads(std::size_t threads) noexcept
    {
        max_threads_.store(threads);
    }

    std::size_t ScanEngine::max_threads() const noexcept
    {
        return max_threads_.load();
    }

    void ScanEngine::publish_running_locked(
        const std::vector<ScanHit>& hits, std::size_t scanned, std::size_t total, std::size_t count, bool truncated)
    {
        snapshot_.state = ScanState::running;
        snapshot_.progress =
            total == 0 ? 1.0f
                       : std::min(1.0f, static_cast<float>(static_cast<double>(scanned) / static_cast<double>(total)));
        snapshot_.scanned_bytes = scanned;
        snapshot_.total_bytes   = total;
        snapshot_.hit_count     = count;
        snapshot_.truncated     = truncated;
        const std::size_t page  = std::min(hits.size(), kDisplayPage);
        snapshot_.hits.assign(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(page));
        snapshot_.result_hits.reset();
    }

    void ScanEngine::publish_results_locked(ScanState state, std::string message)
    {
        snapshot_.state   = state;
        snapshot_.message = std::move(message);
        if (results_)
        {
            snapshot_.hit_count    = results_->count;
            snapshot_.truncated    = results_->truncated;
            const std::size_t page = std::min(results_->hits.size(), kDisplayPage);
            snapshot_.hits.assign(results_->hits.begin(), results_->hits.begin() + static_cast<std::ptrdiff_t>(page));
            snapshot_.result_hits = std::shared_ptr<const std::vector<ScanHit>>(results_, &results_->hits);
        }
        else
        {
            snapshot_.hit_count = 0;
            snapshot_.truncated = false;
            snapshot_.hits.clear();
            snapshot_.result_hits.reset();
        }
    }

    void ScanEngine::finish_success(std::shared_ptr<std::vector<ScanHit>> hits,
                                    std::size_t                           count,
                                    bool                                  truncated,
                                    bool                                  reset_history)
    {
        const std::lock_guard lock(mutex_);
        auto                  set = std::make_shared<const ResultSet>(ResultSet {std::move(*hits), count, truncated});
        if (reset_history)
        {
            history_.clear();
        }
        else if (results_)
        {
            history_.push_back(results_);
            while (history_.size() > kHistoryDepth)
            {
                history_.erase(history_.begin());
            }
        }
        results_           = std::move(set);
        snapshot_.progress = 1.0f;
        publish_results_locked(ScanState::done, "Scan finished.");
    }

    void ScanEngine::run_first(ScanConfig config, MemorySource source, const std::stop_token& token)
    {
        if (is_refinement(config.type))
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "Run a first scan before refining.");
            return;
        }
        if (!source.read || !source.regions)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "No memory source is available.");
            return;
        }

        const std::size_t size = effective_size(config.value_type, config.value);
        if (size == 0)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "The scan value is empty.");
            return;
        }

        const Matcher matcher = Matcher::build(config);

        std::vector<Span> spans;
        try
        {
            spans = build_spans(source.regions(), config.filter);
        }
        catch (...)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "Could not enumerate memory regions.");
            return;
        }

        const std::size_t        total     = total_bytes(spans);
        const std::uint64_t      alignment = config.filter.alignment == 0 ? 1 : config.filter.alignment;
        const std::size_t        cap       = max_stored_hits_.load();
        const std::vector<Shard> shards    = build_shards(spans, alignment);

        std::vector<ShardHits> shard_hits(shards.size());
        {
            const std::lock_guard lock(mutex_);
            merge_page_locked(snapshot_, shard_hits, kDisplayPage, 0, total, 0, false);
        }

        std::atomic<std::size_t> index {0};
        std::atomic<std::size_t> scanned {0};
        std::atomic<std::size_t> stored {0};
        std::atomic<std::size_t> found {0};

        const std::size_t requested = max_threads_.load();
        const std::size_t automatic = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, kMaxScanThreads);
        const std::size_t wanted    = requested == 0 ? automatic : requested;
        std::size_t       thread_count = std::min<std::size_t>(wanted, kMaxScanThreads);
        thread_count                   = std::min<std::size_t>(thread_count, std::max<std::size_t>(shards.size(), 1));

        const auto work = [&]
        {
            // Reused per worker so a chunk read does not allocate every time.
            std::vector<std::byte> buffer;
            for (;;)
            {
                const std::size_t slot = index.fetch_add(1, std::memory_order_relaxed);
                if (slot >= shards.size())
                {
                    break;
                }

                ScannedShard result   = scan_shard_range(shards[slot],
                                                         matcher,
                                                         source,
                                                         alignment,
                                                         size,
                                                         cap,
                                                         buffer,
                                                         stored,
                                                         scanned,
                                                         found,
                                                         token,
                                                         cancel_requested_);
                shard_hits[slot].hits = std::move(result.hits);
                shard_hits[slot].done.store(true, std::memory_order_release);

                {
                    const std::lock_guard lock(mutex_);
                    const std::size_t     count = found.load(std::memory_order_relaxed);
                    merge_page_locked(snapshot_,
                                      shard_hits,
                                      kDisplayPage,
                                      scanned.load(std::memory_order_relaxed),
                                      total,
                                      count,
                                      count > cap);
                }
                if (result.cancelled)
                {
                    break;
                }
            }
        };

        if (thread_count <= 1)
        {
            work();
        }
        else
        {
            std::vector<std::jthread> pool;
            pool.reserve(thread_count);
            for (std::size_t thread = 0; thread < thread_count; ++thread)
            {
                pool.emplace_back(work);
            }
        }

        if (token.stop_requested() || cancel_requested_.load())
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::cancelled, "Scan cancelled; the previous results were kept.");
            return;
        }

        const std::size_t count = found.load(std::memory_order_relaxed);
        auto              next  = std::make_shared<std::vector<ScanHit>>();
        next->reserve(std::min(count, cap));
        for (auto& shard : shard_hits)
        {
            for (auto& hit : shard.hits)
            {
                next->push_back(std::move(hit));
            }
        }
        // Address order makes the result independent of the thread count and of
        // the order in which the shards happened to finish.
        std::sort(next->begin(),
                  next->end(),
                  [](const ScanHit& left, const ScanHit& right)
                  {
                      return left.address < right.address;
                  });

        finish_success(std::move(next), count, count > cap, true);
    }

    void
    ScanEngine::run_next(ScanConfig config, ResultSetPtr previous, MemorySource source, const std::stop_token& token)
    {
        if (config.type == ScanType::unknown_initial_value)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "Unknown initial value is only available for a first scan.");
            return;
        }
        if (!source.read || !previous)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "No previous scan to refine.");
            return;
        }

        const bool        refinement = is_refinement(config.type);
        const std::size_t size       = refinement ? 0 : effective_size(config.value_type, config.value);
        if (!refinement && size == 0)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "The scan value is empty.");
            return;
        }

        auto              next          = std::make_shared<std::vector<ScanHit>>();
        const std::size_t total         = previous->hits.size();
        std::size_t       scanned       = 0;
        std::size_t       count         = 0;
        bool              truncated     = false;
        std::size_t       since_publish = 0;
        const std::size_t cap           = max_stored_hits_.load();

        {
            const std::lock_guard lock(mutex_);
            publish_running_locked(*next, 0, total, 0, false);
        }

        for (const auto& hit : previous->hits)
        {
            if (token.stop_requested() || cancel_requested_.load())
            {
                const std::lock_guard lock(mutex_);
                publish_results_locked(ScanState::cancelled, "Scan cancelled; the previous results were kept.");
                return;
            }

            const std::size_t read_size = refinement ? hit.value.size() : size;
            if (read_size == 0)
            {
                ++scanned;
                continue;
            }

            std::expected<std::vector<std::byte>, process::AccessError> data;
            try
            {
                data = source.read(hit.address, read_size);
            }
            catch (...)
            {
                data = std::unexpected(process::AccessError::internal);
            }

            ++scanned;
            if (data && data->size() >= read_size)
            {
                const std::span<const std::byte> bytes(*data);
                const auto                       window = bytes.subspan(0, read_size);
                const bool                       keep =
                    refinement
                        ? matches_refinement(config.type, config.value_type, hit.value, window)
                        : matches(config.type, config.value_type, window, config.value, config.value_upper, config.hex);
                if (keep)
                {
                    ++count;
                    if (next->size() < cap)
                    {
                        next->push_back(
                            ScanHit {hit.address, std::vector<std::byte>(window.begin(), window.end()), hit.value});
                    }
                    else
                    {
                        truncated = true;
                    }
                }
            }

            if (++since_publish >= kPublishEveryCandidate)
            {
                since_publish = 0;
                const std::lock_guard lock(mutex_);
                publish_running_locked(*next, scanned, total, count, truncated);
            }
        }

        finish_success(std::move(next), count, truncated, false);
    }

} // namespace slopkit::scan
