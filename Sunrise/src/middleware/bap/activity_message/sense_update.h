#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "activity_patch_epoch_parser.h"

namespace sunrise::middleware::bap::activity_message::sense_update {

/** The client reports sensor sense changes. It is the client's answer to the roster update. */
inline constexpr std::uint32_t kMessageType = 6;

/** The body opens with the same 128-bit patch epoch the roster update echoes. */
inline constexpr std::uint8_t kEpochFieldWidth = 64;
/** One literal zero bit follows the epoch. A set bit means the body is not this shape. */
inline constexpr std::uint8_t kLiteralZeroWidth = 1;
/** The client's outer destination bounds the whole body. */
inline constexpr std::size_t kOuterBitCapacity = 514'048;
/** The client's per-group scratch bounds one group substream. */
inline constexpr std::size_t kGroupBitCapacity = 102'400;

/**
 * The recovered prefix of a sensor sense update.
 * The sense delta behind the literal zero has an unresolved width, so the group loop after it
 * cannot be located and everything from the delta on is one bounded tail.
 */
struct SenseUpdate {
    /** Epoch the client believes is current. It must match the one the roster update carried. */
    patch_epoch::PatchEpoch epoch{};
    /** Bits left after the literal zero. Their grammar is unresolved. */
    std::uint32_t tailBits{};
};

/**
 * Parses a sensor sense update as far as its recovered grammar reaches.
 * @param input Activity payload after the envelope.
 * @param update Cleared first, then filled with the epoch and the tail size.
 * @param consumedBits Receives the bits the recovered prefix used.
 * @return True when the epoch and the literal zero were both present and the zero read zero.
 */
[[nodiscard]] bool parse_sense_update(std::span<const std::byte> input,
                                      SenseUpdate& update,
                                      std::size_t& consumedBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sense_update
