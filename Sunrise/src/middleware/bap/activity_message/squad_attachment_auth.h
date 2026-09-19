#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "mission_effect_auth.h"

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

/** Type-26 target selection with the two arena shield resources' neutral authored prefix. */
inline constexpr std::uint32_t kType26Schema = mission_effect::kSchema;
inline constexpr std::uint32_t kType26SquadSelectionSchema = 0x80809157U;
inline constexpr std::size_t kType26EmptyBitCount = mission_effect::kBits;
inline constexpr std::size_t kType26SquadBitCount = 273;
inline constexpr std::size_t kType26MaximumByteCount = (kType26SquadBitCount + 7U) / 8U;

/** No raw actor handles: the native selector enumerates this exact squad ClientRef. */
struct Type26SquadSelection final {
    std::uint32_t registryKey{0x811C9DC5U};
    std::int16_t slotIndex{-1};
    bool active{};
};

/**
 * Encodes the neutral-prefix source selector or its exact empty removal form.
 * Callers must establish the authored prefix and retain ownership of the attachment source.
 * This body changes target attachment; it does not assert damage immunity.
 */
[[nodiscard]] bool encode_type26_squad_selection(const Type26SquadSelection& selection,
                                                 std::span<std::byte> output,
                                                 std::size_t& written,
                                                 std::size_t& writtenBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
