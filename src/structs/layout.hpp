#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "structs/definition.hpp"

namespace slopkit::structs
{
    struct FieldPlacement
    {
        std::uint64_t id {};
        std::uint64_t address {};
        std::size_t   size {};
    };

    struct ReadPlan
    {
        std::vector<std::pair<std::uint64_t, std::size_t>> ranges;
        std::vector<std::uint64_t>                         ids;
    };

    // Resolves every node to an absolute address: a root's address is its
    // evaluated base, a field's is its parent's plus its offset, and a followed
    // pointer's subtree is re-based at the pointer value in `pointer_bases`
    // (indexed by node row; zero means "not followed").
    [[nodiscard]] std::vector<FieldPlacement> place(const Structs&                    structs,
                                                    std::span<const std::string_view> root_names,
                                                    std::span<const std::uint64_t>    root_bases,
                                                    std::span<const std::uint64_t>    pointer_bases,
                                                    std::size_t                       pointer_size);

    [[nodiscard]] ReadPlan plan_reads(const Structs&                  structs,
                                      std::span<const FieldPlacement> placements,
                                      std::span<const std::uint8_t>   visible);

}
