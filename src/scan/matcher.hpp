#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "scan/types.hpp"
#include "scan/value.hpp"

namespace slopkit::scan
{
    struct ScanConfig;

    class Matcher
    {
    public:
        [[nodiscard]] static Matcher build(const ScanConfig& config);

        [[nodiscard]] std::size_t window() const noexcept;

        [[nodiscard]] bool matches_all() const noexcept;

        [[nodiscard]] std::size_t find(std::span<const std::byte> bytes,
                                       std::size_t                offset,
                                       std::size_t                limit,
                                       std::size_t                alignment) const noexcept;

        [[nodiscard]] bool match(std::span<const std::byte> window) const noexcept;

    private:
        enum class Kind : std::uint8_t
        {
            all,
            exact_integer,
            exact_real,
            exact_bytes,
            generic,
        };

        [[nodiscard]] bool        match_integer(std::span<const std::byte> candidate) const noexcept;
        [[nodiscard]] std::size_t find_prefiltered(std::span<const std::byte> bytes,
                                                   std::size_t                offset,
                                                   std::size_t                limit,
                                                   std::size_t                alignment) const noexcept;

        Kind                   kind_ {Kind::generic};
        ScanType               scan_type_ {ScanType::exact_value};
        ValueType              value_type_ {ValueType::int32};
        std::size_t            window_ {};
        std::vector<std::byte> needle_;
        std::uint64_t          integer_needle_ {};
        double                 real_needle_ {};
        ScanValue              value_;
        ScanValue              value_upper_;
        bool                   hex_ {false};
        std::size_t            prefilter_offset_ {};
        std::byte              prefilter_byte_ {};
        bool                   prefilter_ {false};
    };

}
