#pragma once

#include <cstddef>
#include <span>

#include "../../../state/account/account_state.h"
#include "../family4/loadout/definition.h"
#include "layout.h"

namespace sunrise::middleware::datagen::character_record {

/** The family-three character record carries an extra reserved block before the appearance. */
inline constexpr std::size_t kFamily3RecordSize = 3'904;
/** The family-zero banner record carries the same identity and appearance without that block. */
inline constexpr std::size_t kFamily0RecordSize = 3'856;
/** The family-zero banner anchor names the account and the character the banner shows. */
inline constexpr std::size_t kFamily0AnchorSize = 96;

/**
 * Encodes one family-three character record.
 * @param character Validated authored character.
 * @param instances Resolved item instances belonging to that character.
 * @param light Equipment light the banner displays.
 * @param output Exact record storage.
 * @return True when the character and the mapped span fit the native layout.
 */
[[nodiscard]] bool encode_family3(const state::CharacterState& character,
                                  const family4::loadout::ResolvedInstances& instances,
                                  std::int32_t light,
                                  std::span<std::byte> output) noexcept;

/**
 * Encodes one family-zero banner record.
 * @param character Validated authored character.
 * @param instances Resolved item instances belonging to that character.
 * @param light Equipment light the banner displays.
 * @param output Exact record storage.
 * @return True when the character and the mapped span fit the native layout.
 */
[[nodiscard]] bool encode_family0(const state::CharacterState& character,
                                  const family4::loadout::ResolvedInstances& instances,
                                  std::int32_t light,
                                  std::span<std::byte> output) noexcept;

/**
 * Encodes the family-zero banner anchor.
 * The anchor must name the same character as the record beside it or the client rejects both.
 * @param accountSoid Account key.
 * @param characterSoid Character the banner shows.
 * @param output Exact anchor storage.
 * @return True when the mapped span fits the native layout.
 */
[[nodiscard]] bool encode_family0_anchor(std::uint64_t accountSoid,
                                         std::uint64_t characterSoid,
                                         std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::datagen::character_record
