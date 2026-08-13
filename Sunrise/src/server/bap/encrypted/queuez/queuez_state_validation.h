#pragma once

#include <cstdint>

#include "../../../../middleware/queuez/queuez_update.h"
#include "../../../../middleware/queuez/subscription.h"
#include "definition.h"

namespace sunrise::server::bap::encrypted::queuez {

/** @return True when one peer queuez state is canonical for the implemented versions. */
[[nodiscard]] bool valid(const SessionState& state) noexcept;

/**
 * Stages publication of one whole Family-4 snapshot.
 * @param before Current queuez state owned by the peer.
 * @param family Prepared Family-4 snapshot whose object payloads stay borrowed.
 * @param after Gets the state published once the snapshot frame is copied.
 * @return True for a first snapshot or an identical version-zero replay.
 */
[[nodiscard]] bool stage_family4_snapshot(const SessionState& before,
                                          const middleware::queuez::Family& family,
                                           SessionState& after) noexcept;

/** Stages a next-version incremental after-image after an inventory mutation. */
[[nodiscard]] bool stage_family4_replacement(const SessionState& before,
                                             const middleware::queuez::Family& family,
                                             SessionState& after) noexcept;

/**
 * Decides whether one family-zero subscription publishes, and as which kind of frame.
 * Retail sets the full-snapshot flag once per family and adds one to every later push, so a
 * repeat that changes nothing is not sent at all.
 * @param before Current queuez state owned by the peer.
 * @param selectedCharacter Character the family-zero pair names now.
 * @param publish Gets whether a frame is needed.
 * @param incremental Gets whether that frame is an incremental, not a full snapshot.
 * @param after Gets the state published after the whole response transaction.
 * @return True when the request is canonical for the current state.
 */
[[nodiscard]] bool stage_family0_subscription(const SessionState& before,
                                              std::uint64_t selectedCharacter,
                                              bool& publish,
                                              bool& incremental,
                                              SessionState& after) noexcept;

/**
 * Decides whether one validated Family-3 subscription publishes a snapshot.
 * @param before Current queuez state owned by the peer.
 * @param publish Gets whether a svc-123 frame is needed.
 * @param after Gets the state published after the whole response transaction.
 * @return True when the selector belongs to the active post-change root, where that is needed.
 */
[[nodiscard]] bool stage_family3_subscription(const SessionState& before,
                                              const middleware::queuez::Subscription& subscription,
                                              bool& publish,
                                              SessionState& after) noexcept;

/**
 * Stages the fixed first opcode-505 transition for one peer.
 * @param before Current queuez state owned by the peer.
 * @param change Gets the version-one after-image and the account definition.
 * @return True only when a version-zero Family-4 manifest is in place.
 */
[[nodiscard]] bool stage_change_character(const SessionState& before,
                                          ChangeCharacter& change) noexcept;

/**
 * Stages the Family-4 increment that moves the character object to the picked character.
 * @param before Current queuez state owned by the peer.
 * @param selectedCharacterSoid Character key the ws-504 request named.
 * @param select Gets the after-image, both object definitions and both character keys.
 * @return True only when an existing manifest names a different resident character.
 */
[[nodiscard]] bool stage_select_character(const SessionState& before,
                                          std::uint64_t selectedCharacterSoid,
                                          SelectCharacter& select) noexcept;

/** Clears state for the active root. Zero or another root leaves the state unchanged. */
void stage_unsubscription(const SessionState& before,
                          std::uint64_t familyRootSoid,
                          SessionState& after) noexcept;

} // namespace sunrise::server::bap::encrypted::queuez
