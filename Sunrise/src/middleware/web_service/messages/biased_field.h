#pragma once

#include <cstdint>

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::web_service::messages {

/** The vendor opcodes carry their indices as 16-bit signed fields. */
inline constexpr std::uint8_t kBiasedIndexWidth = 16;
/** Their descriptor bias is the signed 16-bit midpoint. */
inline constexpr std::int32_t kBiasedIndexBias = 0x8000;

/**
 * Reads one biased 16-bit index field, the shape every vendor opcode opens with.
 * @param reader Open reader.
 * @param output Receives the logical index.
 * @return True when the field was present.
 */
[[nodiscard]] inline bool read_biased_index(encoding::bits::Reader& reader,
                                            std::int16_t& output) noexcept {
    std::uint64_t stored = 0;
    if (!reader.read(kBiasedIndexWidth, stored)) {
        return false;
    }
    output = static_cast<std::int16_t>(static_cast<std::int32_t>(stored) - kBiasedIndexBias);
    return true;
}

/** The item opcodes carry their quantities as 32-bit signed fields. */
inline constexpr std::uint8_t kBiasedQuantityWidth = 32;
/** Their descriptor bias is the signed 32-bit midpoint. */
inline constexpr std::int64_t kBiasedQuantityBias = 0x8000'0000;

/**
 * Reads one biased 32-bit quantity field.
 * @param reader Open reader.
 * @param output Receives the logical quantity.
 * @return True when the field was present.
 */
[[nodiscard]] inline bool read_biased_quantity(encoding::bits::Reader& reader,
                                               std::int32_t& output) noexcept {
    std::uint64_t stored = 0;
    if (!reader.read(kBiasedQuantityWidth, stored)) {
        return false;
    }
    output = static_cast<std::int32_t>(static_cast<std::int64_t>(stored) - kBiasedQuantityBias);
    return true;
}

} // namespace sunrise::middleware::web_service::messages
