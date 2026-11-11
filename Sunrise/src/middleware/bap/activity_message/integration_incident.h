#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::bap::activity_message::integration_incident {

/** Global incident-table row the Vex block's sacrifice trigger fires (operand 0x0882F18A). */
inline constexpr std::uint32_t kPrimaryTarget = 260;
/** Runtime metadata schema carried by the integration incident payload. */
inline constexpr std::uint32_t kSchema = 0x808087F1U;
/** A u32, a biased three-bit kind and a 64-bit context: 99 bits and five zero padding bits. */
inline constexpr std::size_t kPayloadBits = 99;
inline constexpr std::size_t kPayloadBytes = 13;

/** One integration report. The fields' meanings beyond their widths are not recovered. */
struct Payload final {
    std::uint32_t value{};
    std::int8_t kind{};
    std::uint64_t context{};
};

/**
 * Decodes the fixed schema-0x808087F1 payload, including its required zero padding.
 * @param input Exact payload bytes.
 * @param output Cleared first. Receives the three fields on success.
 * @return True when the whole payload decoded.
 */
[[nodiscard]] inline bool decode(std::span<const std::byte> input, Payload& output) noexcept {
    constexpr std::uint8_t kValueBits = 32;
    constexpr std::uint8_t kKindBits = 3;
    constexpr std::uint8_t kContextBits = 64;
    constexpr std::uint8_t kPaddingBits = 5;
    constexpr std::int32_t kKindBias = 1;
    output = {};
    if (input.size() != kPayloadBytes) {
        return false;
    }
    encoding::bits::Reader reader(input);
    std::uint64_t value = 0;
    std::uint64_t kind = 0;
    std::uint64_t context = 0;
    std::uint64_t padding = 0;
    if (!reader.read(kValueBits, value) || !reader.read(kKindBits, kind)
        || !reader.read(kContextBits, context) || !reader.read(kPaddingBits, padding)
        || padding != 0 || reader.remaining_bits() != 0) {
        return false;
    }
    output.value = static_cast<std::uint32_t>(value);
    output.kind = static_cast<std::int8_t>(static_cast<std::int32_t>(kind) - kKindBias);
    output.context = context;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::integration_incident
