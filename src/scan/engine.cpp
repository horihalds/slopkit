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
        constexpr std::size_t kChunkBytes = 1u << 22;

        constexpr std::size_t kShardBytes = 1u << 25;

        constexpr std::uint64_t kProbeBytes = 4096;

        constexpr std::size_t kPublishEveryCandidate = 1024;

        double elapsed_ms(const std::chrono::steady_clock::time_point& started)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }

        std::string render_value(ValueType type, const ScanValue& value, bool hex)
        {
            const std::vector<std::byte> bytes = encode_value(type, value);
            return format_value(type, bytes, hex);
        }

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

        // One shard's or one batch's own compact store. `max_hits` bounds it so a
        // scan matching most of a large target cannot grow the store past the
        // budget; the shared match counter keeps counting past the bound.
        struct HitStore
        {
            std::vector<ScanHit>      hits;
            std::vector<std::byte>    blob;
            std::size_t               max_hits {};
            std::atomic<std::size_t>* found {};

            void record(std::uint64_t address, std::span<const std::byte> value, std::span<const std::byte> before)
            {
                if (found != nullptr)
                {
                    found->fetch_add(1, std::memory_order_relaxed);
                }
                if (hits.size() >= max_hits)
                {
                    return;
                }
                hits.push_back(make_hit(address, value, before, blob));
            }
        };

        // The share of `allowed` hits that slot `index` of `count` may store. The
        // shares sum to `allowed`, and because they depend on the slot index alone
        // the stored set is identical for every thread count.
        std::size_t slice_for(std::size_t allowed, std::size_t index, std::size_t count)
        {
            if (count == 0)
            {
                return 0;
            }
            return allowed * (index + 1) / count - allowed * index / count;
        }

        // What one stored hit costs: the record itself plus, for a value wider than
        // the inline buffer, its bytes in the store's blob (value and, for a
        // refinement, the before-bytes).
        std::size_t bytes_per_hit(std::size_t width, bool keeps_previous)
        {
            if (width <= ScanHit::kInlineBytes)
            {
                return sizeof(ScanHit);
            }
            return sizeof(ScanHit) + (keeps_previous ? 2 : 1) * width;
        }

        struct ScannedShard
        {
            HitStore    store;
            bool        cancelled {};
            std::size_t unreadable_chunks {};
        };

        ScannedShard scan_shard_range(const Shard&              shard,
                                      const Matcher&            matcher,
                                      const MemorySource&       source,
                                      std::uint64_t             alignment,
                                      std::size_t               size,
                                      std::size_t               max_hits,
                                      std::vector<std::byte>&   buffer,
                                      std::atomic<std::size_t>& scanned,
                                      std::atomic<std::size_t>& found,
                                      const std::stop_token&    token,
                                      const std::atomic<bool>&  cancel_requested)
        {
            ScannedShard result;
            result.store.max_hits     = max_hits;
            result.store.found        = &found;
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
                        result.store.record(
                            address, std::span<const std::byte>(first, size), std::span<const std::byte> {});
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
                    std::uint64_t covered = cursor + bytes.size();
                    if (bytes.empty())
                    {
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

        struct NextBatch
        {
            std::uint64_t begin {};
            std::uint64_t end {};
            std::size_t   first {};
            std::size_t   last {};
        };

        constexpr std::size_t kNextBatchBytes = 4 * 1024;

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

        struct NextContext
        {
            const ScanConfig*         config {};
            const ScanResults*        previous {};
            const MemorySource*       source {};
            bool                      refinement {};
            std::size_t               width {};
            std::atomic<std::size_t>* scanned {};
            std::atomic<std::size_t>* unreadable {};
        };

        void
        record_next_hit(const NextContext& ctx, const ScanHit& hit, std::span<const std::byte> window, HitStore& out)
        {
            const bool keep =
                ctx.refinement
                    ? matches_refinement(ctx.config->type, ctx.config->value_type, ctx.previous->value(hit), window)
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
            out.record(hit.address, window, ctx.previous->value(hit));
        }

        void refine_range(
            const NextContext& ctx, std::size_t first, std::size_t last, std::vector<std::byte>& buffer, HitStore& out)
        {
            const auto&         hits  = ctx.previous->hits;
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
                ctx.scanned->fetch_add(1, std::memory_order_relaxed);
                ctx.unreadable->fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const std::size_t remaining_first = split == first ? first : split;
            const std::size_t remaining       = last - remaining_first + 1;
            if (remaining == 1)
            {
                refine_range(ctx, remaining_first, remaining_first, buffer, out);
                return;
            }
            const std::size_t mid = remaining_first + remaining / 2 - 1;
            refine_range(ctx, remaining_first, mid, buffer, out);
            refine_range(ctx, mid + 1, last, buffer, out);
        }

    }

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
            snapshot_.stored_count = 0;
            snapshot_.capped       = false;
            snapshot_.page         = ScanResults {};
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
        log::info(log::category::scan, std::format("undo restored {} hit(s)", results_ ? results_->hits.size() : 0));
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
        return results_ ? results_->hits.size() : 0;
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

    void ScanEngine::set_stored_result_budget(std::size_t bytes) noexcept
    {
        stored_budget_.store(bytes);
    }

    std::size_t ScanEngine::stored_result_budget() const noexcept
    {
        return stored_budget_.load();
    }

    void ScanEngine::set_retained_result_budget(std::size_t bytes) noexcept
    {
        retained_budget_.store(bytes);
    }

    std::size_t ScanEngine::retained_result_budget() const noexcept
    {
        return retained_budget_.load();
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
        snapshot_.stored_count  = 0;
        snapshot_.capped        = false;
        snapshot_.page          = ScanResults {};
        snapshot_.result_hits.reset();
    }

    void ScanEngine::publish_results_locked(ScanState state, std::string message)
    {
        snapshot_.state   = state;
        snapshot_.message = std::move(message);
        if (results_)
        {
            snapshot_.hit_count    = results_->matches;
            snapshot_.stored_count = results_->hits.size();
            snapshot_.capped       = results_->capped;
            snapshot_.page         = results_->head(kDisplayPage);
            snapshot_.result_hits  = results_;
        }
        else
        {
            snapshot_.hit_count    = 0;
            snapshot_.stored_count = 0;
            snapshot_.capped       = false;
            snapshot_.page         = ScanResults {};
            snapshot_.result_hits.reset();
        }
    }

    void ScanEngine::finish_success(std::shared_ptr<ScanResults> results, bool reset_history)
    {
        const std::lock_guard lock(mutex_);
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
        results_ = std::move(results);

        // The live set and the undo sets share the retained byte budget
        // (kRetainedResultBudget by default): the oldest undo set is dropped
        // first, so a chain of capped refinements cannot multiply the budget. The
        // history depth of 8 stays the upper bound.
        std::size_t retained = results_ ? results_->bytes() : 0;
        for (const auto& set : history_)
        {
            retained += set ? set->bytes() : 0;
        }
        const std::size_t retained_budget = retained_budget_.load();
        while (!history_.empty() && retained > retained_budget)
        {
            retained -= history_.front() ? history_.front()->bytes() : 0;
            history_.erase(history_.begin());
        }

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

        {
            const std::lock_guard lock(mutex_);
            publish_running_locked(0, total, 0);
        }

        // One shard's slice of the byte budget bounds the whole store, and because
        // the slice depends on the slot index alone the stored set does not depend
        // on the thread count or on which shard finished first.
        const std::size_t         allowed     = stored_budget_.load() / bytes_per_hit(size, false);
        const std::size_t         shard_count = std::max<std::size_t>(shards.size(), 1);
        std::vector<ScannedShard> shard_results(shards.size());

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
            std::vector<std::byte> buffer;
            for (;;)
            {
                const std::size_t slot = index.fetch_add(1, std::memory_order_relaxed);
                if (slot >= shards.size())
                {
                    break;
                }

                ScannedShard result    = scan_shard_range(shards[slot],
                                                          matcher,
                                                          source,
                                                          alignment,
                                                          size,
                                                          slice_for(allowed, slot, shard_count),
                                                          buffer,
                                                          scanned,
                                                          found,
                                                          token,
                                                          cancel_requested_);
                const bool   cancelled = result.cancelled;
                shard_results[slot]    = std::move(result);

                {
                    const std::lock_guard lock(mutex_);
                    const std::size_t     count = found.load(std::memory_order_relaxed);
                    publish_running_locked(scanned.load(std::memory_order_relaxed), total, count);
                }
                if (cancelled)
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

        std::size_t stored = 0;
        for (const ScannedShard& shard : shard_results)
        {
            stored += shard.store.hits.size();
        }

        auto results     = std::make_shared<ScanResults>();
        results->matches = count;
        results->hits.reserve(stored);
        for (const ScannedShard& shard : shard_results)
        {
            const std::uint32_t base = static_cast<std::uint32_t>(results->blob.size());
            for (const ScanHit& hit : shard.store.hits)
            {
                ScanHit copy = hit;
                if (copy.wide != 0)
                {
                    copy.blob_offset += base;
                }
                results->hits.push_back(copy);
            }
            results->blob.insert(results->blob.end(), shard.store.blob.begin(), shard.store.blob.end());
        }
        results->capped = results->matches > results->hits.size();
        // Address order makes the result independent of the thread count and of
        // the order in which the shards happened to finish.
        std::sort(results->hits.begin(),
                  results->hits.end(),
                  [](const ScanHit& left, const ScanHit& right)
                  {
                      return left.address < right.address;
                  });

        finish_success(std::move(results), true);

        std::size_t unreadable = 0;
        for (const ScannedShard& shard : shard_results)
        {
            unreadable += shard.unreadable_chunks;
        }
        if (unreadable > 0)
        {
            log::debug(log::category::scan, std::format("first scan: {} unreadable chunk(s) skipped", unreadable));
        }
        if (count > stored)
        {
            log::info(log::category::scan,
                      std::format("first scan capped: {} of {} match(es) stored within {} byte(s)",
                                  stored,
                                  count,
                                  stored_budget_.load()));
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

        const std::size_t total = previous->hits.size();
        const std::size_t width = refinement ? (previous->hits.empty() ? 0 : previous->hits.front().width) : size;

        {
            const std::lock_guard lock(mutex_);
            publish_running_locked(0, total, 0);
        }

        if (total == 0 || width == 0)
        {
            auto empty            = std::make_shared<ScanResults>();
            empty->keeps_previous = refinement;
            empty->capped         = previous->capped;
            finish_success(std::move(empty), false);
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

        const std::vector<NextBatch> batches = build_batches(previous->hits, width);

        // One batch's slice of the byte budget bounds the store; as in a first scan
        // the slice depends on the slot index alone, so the stored set is identical
        // for every thread count.
        const std::size_t allowed     = stored_budget_.load() / bytes_per_hit(width, true);
        const std::size_t batch_count = std::max<std::size_t>(batches.size(), 1);

        NextContext ctx;
        ctx.config     = &config;
        ctx.previous   = previous.get();
        ctx.source     = &source;
        ctx.refinement = refinement;
        ctx.width      = width;
        ctx.scanned    = &scanned;
        ctx.unreadable = &unreadable;

        std::atomic<std::size_t> next_batch {0};
        std::vector<HitStore>    batch_stores(batches.size());
        for (std::size_t slot = 0; slot < batches.size(); ++slot)
        {
            batch_stores[slot].max_hits = slice_for(allowed, slot, batch_count);
            batch_stores[slot].found    = &found;
        }

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
                refine_range(ctx, batches[slot].first, batches[slot].last, buffer, batch_stores[slot]);

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

        std::size_t stored = 0;
        for (const HitStore& slot : batch_stores)
        {
            stored += slot.hits.size();
        }

        auto results            = std::make_shared<ScanResults>();
        results->matches        = count;
        results->keeps_previous = true;
        results->hits.reserve(stored);
        for (const HitStore& slot : batch_stores)
        {
            const std::uint32_t base = static_cast<std::uint32_t>(results->blob.size());
            for (const ScanHit& hit : slot.hits)
            {
                ScanHit copy = hit;
                if (copy.wide != 0)
                {
                    copy.blob_offset += base;
                }
                results->hits.push_back(copy);
            }
            results->blob.insert(results->blob.end(), slot.blob.begin(), slot.blob.end());
        }
        // A refinement of a capped set can only ever see the stored subset, so the
        // result stays marked as capped even when every candidate was kept.
        results->capped = previous->capped || results->matches > results->hits.size();

        finish_success(std::move(results), false);

        const std::size_t unreadable_count = unreadable.load(std::memory_order_relaxed);
        if (unreadable_count > 0)
        {
            log::debug(log::category::scan,
                       std::format("next scan: {} unreadable candidate read(s) skipped", unreadable_count));
        }
        if (count > stored)
        {
            log::info(log::category::scan,
                      std::format("next scan capped: {} of {} match(es) stored within {} byte(s)",
                                  stored,
                                  count,
                                  stored_budget_.load()));
        }
        log::info(
            log::category::scan,
            std::format(
                "next scan finished: {} of {} candidate(s) kept in {:.1f} ms", count, total, elapsed_ms(started)));
    }

}
