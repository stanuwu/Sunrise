#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "mission_effect_auth.h"

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

/**
 * Type-26 attachment Auth: the authored prefix left neutral, then an optional squad selection. The
 * slot type and schema are the mission-effect ones; only the selection tail differs.
 */
inline constexpr std::uint8_t kType26SlotType = mission_effect::kSlotType;
inline constexpr std::uint32_t kType26Schema = mission_effect::kSchema;
inline constexpr std::uint32_t kType26SquadSelectionSchema = 0x80809157U;
inline constexpr std::size_t kType26EmptyBitCount = mission_effect::kBits;
inline constexpr std::size_t kType26SquadBitCount = 273;
inline constexpr std::size_t kType26MaximumByteCount = (kType26SquadBitCount + 7U) / 8U;

/** Selects a squad by its ClientRef; the client resolves its members, never a raw actor handle. */
struct Type26SquadSelection final {
    std::uint32_t registryKey{0x811C9DC5U};
    std::int16_t slotIndex{-1};
    bool active{};
};

/**
 * Encodes the squad selection, or the empty body that clears it. The caller owns the attachment
 * source; the body only moves the attachment and does not make the squad immune to damage.
 */
[[nodiscard]] bool encode_type26_squad_selection(const Type26SquadSelection& selection,
                                                 std::span<std::byte> output,
                                                 std::size_t& written,
                                                 std::size_t& writtenBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
