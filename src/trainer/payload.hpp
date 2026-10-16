#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "scan/types.hpp"

namespace slopkit::trainer
{
    // The payload rides after the last byte of a copied runtime template: both the
    // ELF and the PE loader ignore trailing bytes, so one file carries the runtime
    // and the table it was generated from. The finger is read first, as the last
    // `kFingerSize` bytes of the file, and gives the payload's size.
    inline constexpr std::string_view kPayloadMagic {"SLPKTRN1"};
    inline constexpr std::string_view kFingerMagic {"SLPKTRN2"};
    inline constexpr std::uint32_t    kPayloadVersion {1};
    inline constexpr std::size_t      kFingerSize {16};

    inline constexpr std::uint32_t kTargetLinux {0};
    inline constexpr std::uint32_t kTargetWindows {1};

    inline constexpr std::uint8_t kEntryFrozen {1u << 0};
    inline constexpr std::uint8_t kEntryHex {1u << 1};

    // README.md owns the exit code list the runtime implements.
    inline constexpr int kExitOk {0};
    inline constexpr int kExitUsage {1};
    inline constexpr int kExitNoPayload {2};
    inline constexpr int kExitNoProcess {3};
    inline constexpr int kExitAccess {4};

    enum class TargetOs
    {
        linux,
        windows,
    };

    struct PayloadEntry
    {
        std::string            description;
        scan::ValueType        type {scan::ValueType::int32};
        bool                   frozen {false};
        bool                   hex {false};
        std::vector<std::byte> bytes;
        std::string            expression;
        std::uint64_t          last_address {0};
    };

    struct Payload
    {
        std::uint32_t             target_os {kTargetLinux};
        std::uint32_t             pointer_size {8};
        std::string               process_name;
        std::vector<PayloadEntry> entries;
    };

    [[nodiscard]] std::string_view target_name(TargetOs target) noexcept;

    // Serialises the records and appends the trailing finger; the generator
    // appends exactly these bytes to a copied template.
    [[nodiscard]] std::vector<std::byte> encode_payload(const Payload& payload);

    // Reads the payload out of a whole trainer image (template bytes plus tail).
    [[nodiscard]] std::expected<Payload, std::string> decode_payload(std::span<const std::byte> image);

}
