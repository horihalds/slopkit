#include "scan/types.hpp"

#include <iterator>

namespace slopkit::scan
{

    std::string_view describe(ValueType type) noexcept
    {
        const auto index = static_cast<std::size_t>(type);
        return index < std::size(kValueTypeNames) ? kValueTypeNames[index] : "Unknown";
    }

    std::string_view describe(ScanType type) noexcept
    {
        const auto index = static_cast<std::size_t>(type);
        return index < std::size(kScanTypeNames) ? kScanTypeNames[index] : "Unknown";
    }

    std::string_view describe(ScanState state) noexcept
    {
        switch (state)
        {
        case ScanState::idle:
            return "idle";
        case ScanState::running:
            return "running";
        case ScanState::done:
            return "done";
        case ScanState::failed:
            return "failed";
        case ScanState::cancelled:
            return "cancelled";
        }
        return "unknown";
    }

} // namespace slopkit::scan
