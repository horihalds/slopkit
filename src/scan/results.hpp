#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace slopkit::scan
{
    // One stored hit: a fixed-size, trivially copyable record with no per-hit
    // allocation. Values up to kInlineBytes travel inside the record; a wider one
    // (a string or byte-array needle) lives in ScanResults::blob and is addressed
    // by blob_offset, with the previous bytes right after the value.
    struct ScanHit
    {
        static constexpr std::size_t kInlineBytes = 8;

        std::uint64_t address {};

        // A 16-bit width keeps a long string needle exact while the whole record
        // still fits one cache-line pair.
        std::uint32_t width    : 16 {};
        std::uint32_t wide     : 1 {};
        std::uint32_t readable : 1 {};
        std::uint32_t reserved : 14 {};

        std::uint32_t                       blob_offset {};
        std::array<std::byte, kInlineBytes> value {};
        std::array<std::byte, kInlineBytes> previous {};
    };

    static_assert(std::is_trivially_copyable_v<ScanHit>);
    static_assert(sizeof(ScanHit) == 32);

    // Every result set of one scan. `hits` is the address-ordered stored set,
    // `matches` is every match the scan found (>= hits.size() when capped).
    struct ScanResults
    {
        std::vector<ScanHit>   hits;
        std::vector<std::byte> blob;
        std::size_t            matches {};
        bool                   capped {};
        bool                   keeps_previous {};

        [[nodiscard]] std::span<const std::byte> value(const ScanHit& hit) const noexcept
        {
            if (hit.width == 0)
            {
                return {};
            }
            if (hit.wide == 0)
            {
                return std::span<const std::byte>(hit.value.data(), hit.width);
            }
            return std::span<const std::byte>(blob).subspan(hit.blob_offset, hit.width);
        }

        [[nodiscard]] std::span<const std::byte> previous(const ScanHit& hit) const noexcept
        {
            if (!keeps_previous || hit.width == 0)
            {
                return {};
            }
            if (hit.wide == 0)
            {
                return std::span<const std::byte>(hit.previous.data(), hit.width);
            }
            return std::span<const std::byte>(blob).subspan(hit.blob_offset + hit.width, hit.width);
        }

        [[nodiscard]] std::size_t bytes() const noexcept
        {
            return hits.size() * sizeof(ScanHit) + blob.size();
        }

        // The first `rows` hits as a self-contained set: the wide bytes they
        // reference are re-packed into the returned blob and their offsets
        // rewritten, so a page can never dangle into the full set.
        [[nodiscard]] ScanResults head(std::size_t rows) const
        {
            ScanResults page;
            page.keeps_previous = keeps_previous;

            const std::size_t count = std::min(rows, hits.size());
            page.hits.assign(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(count));
            page.matches = count;
            for (ScanHit& hit : page.hits)
            {
                if (hit.wide == 0)
                {
                    continue;
                }
                const std::size_t stored = keeps_previous ? 2 * hit.width : hit.width;
                const auto        source = std::span<const std::byte>(blob).subspan(hit.blob_offset, stored);
                hit.blob_offset          = static_cast<std::uint32_t>(page.blob.size());
                page.blob.insert(page.blob.end(), source.begin(), source.end());
            }
            return page;
        }
    };

    // Builds one record with `value` as the current bytes and, for a refinement,
    // `before` as the previous bytes (empty on a first scan). A value wider than
    // the inline buffer is appended to `blob`.
    [[nodiscard]] inline ScanHit make_hit(std::uint64_t              address,
                                          std::span<const std::byte> value,
                                          std::span<const std::byte> before,
                                          std::vector<std::byte>&    blob)
    {
        ScanHit hit {};
        hit.address  = address;
        hit.width    = static_cast<std::uint32_t>(value.size());
        hit.readable = value.empty() ? 0 : 1;
        if (value.size() <= ScanHit::kInlineBytes)
        {
            std::memcpy(hit.value.data(), value.data(), value.size());
            std::memcpy(hit.previous.data(), before.data(), before.size());
        }
        else
        {
            hit.wide        = 1;
            hit.blob_offset = static_cast<std::uint32_t>(blob.size());
            blob.insert(blob.end(), value.begin(), value.end());
            blob.insert(blob.end(), before.begin(), before.end());
        }
        return hit;
    }
}
