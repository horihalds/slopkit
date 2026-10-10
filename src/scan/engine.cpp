#include "scan/engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <format>
#include <span>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"
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

        // When a whole chunk read fails there is no readable prefix to resume
        // after, so the cursor probes forward one page at a time to find where
        // reading resumes. This keeps a wholly unreadable region at one read per
        // page instead of one read per candidate (alignment can be 1).
        constexpr std::uint64_t kProbeBytes = 4096;

        // How often the worker publishes progress, to limit lock churn.
        constexpr std::size_t kPublishEveryCandidate = 1024;

        double elapsed_ms(const std::chrono::steady_clock::time_point& started)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }

        // Renders a comparison value the way the table shows it.
        std::string render_value(ValueType type, const ScanValue& value, bool hex)
        {
            const std::vector<std::byte> bytes = encode_value(type, value);
            return format_value(type, bytes, hex);
        }

        // A compact, single-line rendering of the scan configuration.
        std::string describe_config(const ScanConfig& config)
        {
            std::string text = std::format("{} value_type={} value={}",
                                           describe(config.type),
                                           describe(config.value_type),
                                           render_value(config.value_type, config.value, config.hex));
            if (config.type == ScanType::value_between)
            {
                text += std::format(" upper={}", render_value(config.value_type, config.value_upper, config.hex));
            }
            text += std::format(" alignment={}", config.filter.alignment);
            if (config.filter.start != 0 || config.filter.stop != 0)
            {
                text += std::format(" range={:X}-{:X}", config.filter.start, config.filter.stop);
            }
            if (config.filter.writable)
            {
                text += " writable";
            }
            if (config.filter.executable)
            {
                text += " executable";
            }
            if (config.filter.copy_on_write)
            {
                text += " copy_on_write";
            }
            return text;
        }

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

        struct ScannedShard
        {
            std::vector<ScanHit> hits;
            bool                 cancelled {};
            std::size_t          unreadable_chunks {};
        };

        // Walks one shard in chunks, reading one extra window across the chunk
        // and shard boundaries so windows that straddle them are not skipped.
        // Candidates only start inside the chunk, so `scanned` accounting is
        // unchanged.
        ScannedShard scan_shard_range(const Shard&              shard,
                                      const Matcher&            matcher,
                                      const MemorySource&       source,
                                      std::uint64_t             alignment,
                                      std::size_t               size,
                                      std::vector<std::byte>&   buffer,
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
                        result.hits.push_back(ScanHit {address, std::vector<std::byte>(first, first + size), {}});
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
                else
                {
                    ++result.unreadable_chunks;
                }

                std::uint64_t next = cursor + chunk;
                if (bytes.size() < read_size)
                {
                    // The read stopped early. Resume at the first candidate on the
                    // grid whose window is not fully inside the bytes we already
                    // have, so a window straddling a hole is re-checked and nothing
                    // past the hole is skipped; never advance by less than the
                    // alignment, so the cursor always progresses.
                    std::uint64_t covered = cursor + bytes.size();
                    if (bytes.empty())
                    {
                        // Nothing was read: probe forward a page at a time to find
                        // where reading resumes, capped at this chunk. A wholly
                        // unreadable chunk therefore costs one read per page, not
                        // one per candidate.
                        const std::uint64_t chunk_end = cursor + chunk;
                        std::uint64_t       probe     = cursor;
                        while (probe < chunk_end)
                        {
                            probe               = std::min<std::uint64_t>(probe + kProbeBytes, chunk_end);
                            const bool readable = [&]
                            {
                                if (source.read_into)
                                {
                                    std::array<std::byte, 64> scratch {};
                                    const std::size_t         probe_size = std::min<std::size_t>(size, scratch.size());
                                    try
                                    {
                                        const auto count =
                                            source.read_into(probe, std::span<std::byte>(scratch.data(), probe_size));
                                        return count && *count > 0;
                                    }
                                    catch (...)
                                    {
                                        return false;
                                    }
                                }
                                try
                                {
                                    const auto data = source.read(probe, size);
                                    return data && !data->empty();
                                }
                                catch (...)
                                {
                                    return false;
                                }
                            }();
                            if (readable)
                            {
                                covered = probe;
                                break;
                            }
                        }
                        if (probe >= chunk_end && covered <= cursor)
                        {
                            covered = chunk_end;
                        }
                    }

                    std::uint64_t resume = alignment;
                    if (covered > cursor)
                    {
                        const std::uint64_t available = covered - cursor;
                        if (available >= size)
                        {
                            const std::uint64_t last_start = available - size;
                            resume                         = last_start - (last_start % alignment) + alignment;
                        }
                        else
                        {
                            resume = available;
                        }
                    }
                    resume = std::max<std::uint64_t>(resume, alignment);
                    next   = std::min<std::uint64_t>(cursor + chunk, cursor + resume);
                }
                scanned.fetch_add(next - cursor, std::memory_order_relaxed);
                cursor = next;
            }
            return result;
        }

        // A refinement reads the candidates in bounded, address-sorted runs, so
        // a dense result set costs one target read per run instead of one per
        // candidate. The run's read window is [begin, end) and it covers the
        // hits in [first, last].
        struct NextBatch
        {
            std::uint64_t begin {};
            std::uint64_t end {};
            std::size_t   first {};
            std::size_t   last {};
        };

        constexpr std::size_t kNextBatchBytes = 4 * 1024;

        // Groups address-sorted hits into runs whose read window stays within
        // kNextBatchBytes; a single candidate wider than that forms its own run.
        std::vector<NextBatch> build_batches(const std::vector<ScanHit>& hits, std::size_t width)
        {
            std::vector<NextBatch> batches;
            std::size_t            index = 0;
            while (index < hits.size())
            {
                NextBatch batch;
                batch.begin = hits[index].address;
                batch.end   = hits[index].address + width;
                batch.first = index;
                batch.last  = index;
                while (batch.last + 1 < hits.size())
                {
                    const std::size_t   candidate     = batch.last + 1;
                    const std::uint64_t candidate_end = hits[candidate].address + width;
                    if (candidate_end - batch.begin > kNextBatchBytes)
                    {
                        break;
                    }
                    batch.end  = std::max(batch.end, candidate_end);
                    batch.last = candidate;
                }
                batches.push_back(batch);
                index = batch.last + 1;
            }
            return batches;
        }

        // The read-only state each refinement worker shares.
        struct NextContext
        {
            const ScanConfig*           config {};
            const std::vector<ScanHit>* hits {};
            const MemorySource*         source {};
            bool                        refinement {};
            std::size_t                 width {};
            std::atomic<std::size_t>*   scanned {};
            std::atomic<std::size_t>*   found {};
            std::atomic<std::size_t>*   unreadable {};
        };

        // Compares one candidate read out of the batch buffer and records it,
        // updating the shared counters exactly like the old per-candidate loop.
        void record_next_hit(const NextContext&         ctx,
                             const ScanHit&             hit,
                             std::span<const std::byte> window,
                             std::vector<ScanHit>&      out)
        {
            const bool keep = ctx.refinement
                                ? matches_refinement(ctx.config->type, ctx.config->value_type, hit.value, window)
                                : matches(ctx.config->type,
                                          ctx.config->value_type,
                                          window,
                                          ctx.config->value,
                                          ctx.config->value_upper,
                                          ctx.config->hex);
            ctx.scanned->fetch_add(1, std::memory_order_relaxed);
            if (!keep)
            {
                return;
            }
            ctx.found->fetch_add(1, std::memory_order_relaxed);
            out.push_back(ScanHit {hit.address, std::vector<std::byte>(window.begin(), window.end()), hit.value});
        }

        // Refines hits[first..last] with one read of their whole window. A short
        // or failed read still matches the candidates fully inside the readable
        // prefix and retries the rest in halves, so one unmapped address cannot
        // discard its neighbours; a lone candidate that stays unreadable is
        // counted and dropped, exactly like the old per-candidate path.
        void refine_range(const NextContext&      ctx,
                          std::size_t             first,
                          std::size_t             last,
                          std::vector<std::byte>& buffer,
                          std::vector<ScanHit>&   out)
        {
            const auto&         hits  = *ctx.hits;
            const std::uint64_t begin = hits[first].address;
            std::uint64_t       end   = begin + ctx.width;
            for (std::size_t i = first; i <= last; ++i)
            {
                end = std::max(end, hits[i].address + ctx.width);
            }
            const std::size_t span = static_cast<std::size_t>(end - begin);

            std::size_t readable = 0;
            if (ctx.source->read_into)
            {
                if (buffer.size() < span)
                {
                    buffer.resize(span);
                }
                std::expected<std::size_t, process::AccessError> count;
                try
                {
                    count = ctx.source->read_into(begin, std::span<std::byte>(buffer.data(), span));
                }
                catch (...)
                {
                    count = std::unexpected(process::AccessError::internal);
                }
                if (count && *count > 0)
                {
                    readable = *count;
                }
            }
            else
            {
                std::expected<std::vector<std::byte>, process::AccessError> owned;
                try
                {
                    owned = ctx.source->read(begin, span);
                }
                catch (...)
                {
                    owned = std::unexpected(process::AccessError::internal);
                }
                if (owned && !owned->empty())
                {
                    if (buffer.size() < owned->size())
                    {
                        buffer.resize(owned->size());
                    }
                    std::copy(owned->begin(), owned->end(), buffer.begin());
                    readable = owned->size();
                }
            }

            const std::span<const std::byte> bytes(buffer.data(), std::min(readable, buffer.size()));
            std::size_t                      split = first;
            while (split <= last && hits[split].address + ctx.width <= begin + readable)
            {
                const std::size_t offset = static_cast<std::size_t>(hits[split].address - begin);
                record_next_hit(ctx, hits[split], bytes.subspan(offset, ctx.width), out);
                ++split;
            }
            if (split > last)
            {
                return;
            }
            if (first == last)
            {
                // The only candidate could not be read in full.
                ctx.scanned->fetch_add(1, std::memory_order_relaxed);
                ctx.unreadable->fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const std::size_t remaining_first = split == first ? first : split;
            const std::size_t remaining       = last - remaining_first + 1;
            if (remaining == 1)
            {
                // Only the tail is left; retry it on its own.
                refine_range(ctx, remaining_first, remaining_first, buffer, out);
                return;
            }
            const std::size_t mid = remaining_first + remaining / 2 - 1;
            refine_range(ctx, remaining_first, mid, buffer, out);
            refine_range(ctx, mid + 1, last, buffer, out);
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

        log::info(log::category::scan, std::format("first scan: {}", describe_config(config)));

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

        log::info(log::category::scan, std::format("next scan: {}", describe_config(config)));

        ResultSetPtr previous;
        MemorySource source;
        {
            const std::lock_guard lock(mutex_);
            if (!results_)
            {
                snapshot_.state   = ScanState::failed;
                snapshot_.message = "Run a first scan before refining.";
                log::warning(log::category::scan, "next scan rejected: no previous results");
                return;
            }
            previous        = results_;
            source          = source_;
            config_         = config;
            snapshot_.state = ScanState::running;
            snapshot_.message.clear();
            // A refinement must not expose the previous set as a running page.
            snapshot_.hits.clear();
            snapshot_.result_hits.reset();
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
            log::info(log::category::scan, "undo requested with no history");
            return;
        }
        results_ = history_.back();
        history_.pop_back();
        publish_results_locked(ScanState::done, "Undid the last scan.");
        log::info(log::category::scan, std::format("undo restored {} hit(s)", results_ ? results_->size() : 0));
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
        log::info(log::category::scan, "scan cancel requested");
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
        log::info(log::category::scan, "scan reset");
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
        return results_ ? results_->size() : 0;
    }

    bool ScanEngine::has_results() const
    {
        const std::lock_guard lock(mutex_);
        return results_ != nullptr;
    }

    bool ScanEngine::can_undo() const
    {
        const std::lock_guard lock(mutex_);
        return !history_.empty();
    }

    void ScanEngine::set_max_threads(std::size_t threads) noexcept
    {
        max_threads_.store(threads);
    }

    std::size_t ScanEngine::max_threads() const noexcept
    {
        return max_threads_.load();
    }

    void ScanEngine::publish_running_locked(std::size_t scanned, std::size_t total, std::size_t count)
    {
        snapshot_.state = ScanState::running;
        snapshot_.progress =
            total == 0 ? 1.0f
                       : std::min(1.0f, static_cast<float>(static_cast<double>(scanned) / static_cast<double>(total)));
        snapshot_.scanned_bytes = scanned;
        snapshot_.total_bytes   = total;
        snapshot_.hit_count     = count;
        snapshot_.hits.clear();
        snapshot_.result_hits.reset();
    }

    void ScanEngine::publish_results_locked(ScanState state, std::string message)
    {
        snapshot_.state   = state;
        snapshot_.message = std::move(message);
        if (results_)
        {
            snapshot_.hit_count    = results_->size();
            const std::size_t page = std::min(results_->size(), kDisplayPage);
            snapshot_.hits.assign(results_->begin(), results_->begin() + static_cast<std::ptrdiff_t>(page));
            snapshot_.result_hits = results_;
        }
        else
        {
            snapshot_.hit_count = 0;
            snapshot_.hits.clear();
            snapshot_.result_hits.reset();
        }
    }

    void ScanEngine::finish_success(std::shared_ptr<std::vector<ScanHit>> hits, bool reset_history)
    {
        const std::lock_guard lock(mutex_);
        auto                  set = std::make_shared<const std::vector<ScanHit>>(std::move(*hits));
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
        const auto started = std::chrono::steady_clock::now();

        if (is_refinement(config.type))
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "Run a first scan before refining.");
            log::error(log::category::scan, "first scan failed: a refinement needs a previous scan");
            return;
        }
        if (!source.read || !source.regions)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "No memory source is available.");
            log::error(log::category::scan, "first scan failed: no memory source is available");
            return;
        }

        const std::size_t size = effective_size(config.value_type, config.value);
        if (size == 0)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "The scan value is empty.");
            log::error(log::category::scan, "first scan failed: the scan value is empty");
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
            log::error(log::category::scan, "first scan failed: could not enumerate memory regions");
            return;
        }

        const std::size_t        total     = total_bytes(spans);
        const std::uint64_t      alignment = config.filter.alignment == 0 ? 1 : config.filter.alignment;
        const std::vector<Shard> shards    = build_shards(spans, alignment);

        std::vector<std::vector<ScanHit>> shard_hits(shards.size());
        std::vector<std::size_t>          shard_unreadable(shards.size());
        {
            const std::lock_guard lock(mutex_);
            publish_running_locked(0, total, 0);
        }

        std::atomic<std::size_t> index {0};
        std::atomic<std::size_t> scanned {0};
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

                ScannedShard result = scan_shard_range(
                    shards[slot], matcher, source, alignment, size, buffer, scanned, found, token, cancel_requested_);
                shard_hits[slot]       = std::move(result.hits);
                shard_unreadable[slot] = result.unreadable_chunks;

                {
                    const std::lock_guard lock(mutex_);
                    const std::size_t     count = found.load(std::memory_order_relaxed);
                    publish_running_locked(scanned.load(std::memory_order_relaxed), total, count);
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
            log::info(log::category::scan, std::format("first scan cancelled after {:.1f} ms", elapsed_ms(started)));
            return;
        }

        const std::size_t count = found.load(std::memory_order_relaxed);
        auto              next  = std::make_shared<std::vector<ScanHit>>();
        next->reserve(count);
        for (auto& shard : shard_hits)
        {
            for (auto& hit : shard)
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

        finish_success(std::move(next), true);

        std::size_t unreadable = 0;
        for (const std::size_t value : shard_unreadable)
        {
            unreadable += value;
        }
        if (unreadable > 0)
        {
            log::debug(log::category::scan, std::format("first scan: {} unreadable chunk(s) skipped", unreadable));
        }

        log::info(
            log::category::scan,
            std::format(
                "first scan finished: {} hit(s), {} byte(s) scanned in {:.1f} ms", count, total, elapsed_ms(started)));
    }

    void
    ScanEngine::run_next(ScanConfig config, ResultSetPtr previous, MemorySource source, const std::stop_token& token)
    {
        const auto started = std::chrono::steady_clock::now();

        if (config.type == ScanType::unknown_initial_value)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "Unknown initial value is only available for a first scan.");
            log::error(log::category::scan, "next scan failed: unknown initial value needs a first scan");
            return;
        }
        if (!source.read || !previous)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "No previous scan to refine.");
            log::error(log::category::scan, "next scan failed: no previous scan to refine");
            return;
        }

        const bool        refinement = is_refinement(config.type);
        const std::size_t size       = refinement ? 0 : effective_size(config.value_type, config.value);
        if (!refinement && size == 0)
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::failed, "The scan value is empty.");
            log::error(log::category::scan, "next scan failed: the scan value is empty");
            return;
        }

        const std::size_t total = previous->size();
        const std::size_t width = refinement ? (previous->empty() ? 0 : previous->front().value.size()) : size;

        {
            const std::lock_guard lock(mutex_);
            publish_running_locked(0, total, 0);
        }

        if (total == 0 || width == 0)
        {
            // Nothing to refine; a zero width is the old loop's "read_size == 0"
            // case, where every candidate is dropped.
            finish_success(std::make_shared<std::vector<ScanHit>>(), false);
            log::info(
                log::category::scan,
                std::format(
                    "next scan finished: {} of {} candidate(s) kept in {:.1f} ms", 0, total, elapsed_ms(started)));
            return;
        }

        std::atomic<std::size_t> scanned {0};
        std::atomic<std::size_t> found {0};
        std::atomic<std::size_t> unreadable {0};
        std::atomic<bool>        cancelled {false};

        const std::vector<NextBatch> batches = build_batches(*previous, width);

        NextContext ctx;
        ctx.config     = &config;
        ctx.hits       = previous.get();
        ctx.source     = &source;
        ctx.refinement = refinement;
        ctx.width      = width;
        ctx.scanned    = &scanned;
        ctx.found      = &found;
        ctx.unreadable = &unreadable;

        std::atomic<std::size_t>          next_batch {0};
        std::vector<std::vector<ScanHit>> batch_hits(batches.size());

        const auto work = [&]
        {
            std::vector<std::byte> buffer;
            std::size_t            since_publish = 0;
            while (!cancelled.load(std::memory_order_relaxed))
            {
                if (token.stop_requested() || cancel_requested_.load())
                {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                const std::size_t slot = next_batch.fetch_add(1, std::memory_order_relaxed);
                if (slot >= batches.size())
                {
                    return;
                }
                refine_range(ctx, batches[slot].first, batches[slot].last, buffer, batch_hits[slot]);

                since_publish += batches[slot].last - batches[slot].first + 1;
                if (since_publish >= kPublishEveryCandidate)
                {
                    since_publish = 0;
                    const std::lock_guard lock(mutex_);
                    publish_running_locked(
                        scanned.load(std::memory_order_relaxed), total, found.load(std::memory_order_relaxed));
                }
            }
        };

        const std::size_t requested = max_threads_.load();
        const std::size_t automatic = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, kMaxScanThreads);
        const std::size_t wanted    = requested == 0 ? automatic : requested;
        std::size_t       threads   = std::min<std::size_t>(wanted, kMaxScanThreads);
        threads                     = std::min<std::size_t>(threads, std::max<std::size_t>(batches.size(), 1));

        if (threads <= 1)
        {
            work();
        }
        else
        {
            std::vector<std::jthread> pool;
            pool.reserve(threads);
            for (std::size_t i = 0; i < threads; ++i)
            {
                pool.emplace_back(work);
            }
        }

        if (cancelled.load(std::memory_order_relaxed) || token.stop_requested() || cancel_requested_.load())
        {
            const std::lock_guard lock(mutex_);
            publish_results_locked(ScanState::cancelled, "Scan cancelled; the previous results were kept.");
            log::info(log::category::scan, std::format("next scan cancelled after {:.1f} ms", elapsed_ms(started)));
            return;
        }

        const std::size_t count = found.load(std::memory_order_relaxed);
        auto              next  = std::make_shared<std::vector<ScanHit>>();
        next->reserve(count);
        for (auto& slot : batch_hits)
        {
            for (auto& hit : slot)
            {
                next->push_back(std::move(hit));
            }
        }

        finish_success(std::move(next), false);

        const std::size_t unreadable_count = unreadable.load(std::memory_order_relaxed);
        if (unreadable_count > 0)
        {
            log::debug(log::category::scan,
                       std::format("next scan: {} unreadable candidate read(s) skipped", unreadable_count));
        }
        log::info(
            log::category::scan,
            std::format(
                "next scan finished: {} of {} candidate(s) kept in {:.1f} ms", count, total, elapsed_ms(started)));
    }

} // namespace slopkit::scan
