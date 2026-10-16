#pragma once

#include <cstdint>
#include <span>

#include "../../../../middleware/queuez/queuez_update.h"
#include "../../../../middleware/queuez/subscription.h"
#include "definition.h"

namespace sunrise::state {
struct PendingRecordRewardGrant;
}

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

/** Replaces an active peer's Family-4 manifest with a full next-version snapshot. */
[[nodiscard]] bool stage_family4_refresh(const SessionState& before,
                                         const middleware::queuez::Family& family,
                                         SessionState& after) noexcept;

/**
 * Decides whether one family-zero subscription publishes, and as which kind of frame.
 * A repeat naming the character the pair already holds reports no publish and no version bump.
 * Both callers send anyway, so the answer is which frame to build, not whether to answer.
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
 * @param subscription Family selector the Client asked for.
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

/**
 * Stages one Family-4 version increment without changing its resident manifest.
 * @param before Current queuez state owned by the peer.
 * @param characterSoid Selected character whose equipment changed.
 * @param swap Gets the checked version after-image and resident definition.
 * @return True only when the named character object is resident in active Family 4.
 */
[[nodiscard]] bool stage_equipment_swap(const SessionState& before,
                                        std::uint64_t characterSoid,
                                        EquipmentSwap& swap) noexcept;

/**
 * Stages one same-character Family-0 appearance-record increment after an equipment swap.
 * Family zero already owns the record, so the character key is preserved and only its version
 * ladder advances.
 *
 * @param before Peer state after the corresponding Family-4 character upsert.
 * @param characterSoid Selected character whose rendered equipment changed.
 * @param refresh Gets the exact combined Family-4/Family-0 after-image.
 * @return True only when active Family zero already names the same character.
 */
[[nodiscard]] bool stage_character_appearance_refresh(const SessionState& before,
                                                      std::uint64_t characterSoid,
                                                      CharacterAppearanceRefresh& refresh) noexcept;

/**
 * Advances the active Family-3 ladder for one appearance refresh.
 * @param before Peer state after any paired Family-4 and Family-0 frames.
 * @param characterSoid Character whose Family-3 record changes.
 * @param includeRoster Whether the account roster body changes in the same increment.
 * @param refresh Gets the exact +1 Family-3 after-image.
 * @return True only for an active roster store rooted with the peer's account family.
 */
[[nodiscard]] bool stage_roster_appearance_refresh(const SessionState& before,
                                                   std::uint64_t characterSoid,
                                                   bool includeRoster,
                                                   RosterAppearanceRefresh& refresh) noexcept;

/**
 * Stages one Family-4 version increment without changing its resident manifest.
 * @param before Current active peer state.
 * @param accountSoid Account root the peer's family is rooted with.
 * @param characterSoid Selected resident character owning the changed item.
 * @param targetInstanceSoid Existing resident item-instance key to upsert.
 * @param updatesAccount True when the account object rides the same increment.
 * @param socketPlug Gets the exact +1 version and item-instance schema id.
 * @return True when both the selected character and target instance are resident exactly once.
 */
[[nodiscard]] bool stage_socket_plug(const SessionState& before,
                                     std::uint64_t accountSoid,
                                     std::uint64_t characterSoid,
                                     std::uint64_t targetInstanceSoid,
                                     bool updatesAccount,
                                     SocketPlug& socketPlug) noexcept;

/** Stages one resident subclass item-instance upsert without changing the Family-4 manifest. */
[[nodiscard]] bool stage_subclass_selection(const SessionState& before,
                                            std::uint64_t accountSoid,
                                            std::uint64_t characterSoid,
                                            std::uint64_t subclassInstanceSoid,
                                            SubclassSelection& selection) noexcept;

/**
 * Stages one Family-4 increment that adds a new resident item and updates its character.
 *
 * @param before Current active peer state.
 * @param accountSoid Account root the peer's family is rooted with.
 * @param characterSoid Selected resident character receiving the item.
 * @param acquiredInstanceSoid Fresh item-instance SOID absent from the resident manifest.
 * @param updatesAccount True when the account object rides the same increment.
 * @param acquisition Gets the exact +1 version and appended resident after-image.
 * @return True when both schemas resolve and the manifest has one free resident slot.
 */
[[nodiscard]] bool stage_item_acquisition(const SessionState& before,
                                          std::uint64_t accountSoid,
                                          std::uint64_t characterSoid,
                                          std::uint64_t acquiredInstanceSoid,
                                          bool updatesAccount,
                                          ItemAcquisition& acquisition) noexcept;

/**
 * Stages one Family-4 version increment for a full resident account-object upsert.
 * A profile row with a nonzero action-source SOID must already be resident when its stack grows,
 * or is appended once when Collections creates it. Currency rows keep a zero SOID.
 * @param before Current active peer state.
 * @param accountSoid Account root receiving the profile stack.
 * @param acquiredInstanceSoid Profile action-source key, or zero for a non-actionable stack.
 * @param actionSource Shared installed-build classification from the prepared State mutation.
 * @param appended True when the account mutation created a new profile row.
 * @param acquisition Gets the exact +1 version, definitions, and optional manifest append.
 * @return True when the account/root and optional profile resident transition are exact.
 */
[[nodiscard]] bool stage_profile_item_acquisition(const SessionState& before,
                                                  std::uint64_t accountSoid,
                                                  std::uint64_t acquiredInstanceSoid,
                                                  bool actionSource,
                                                  bool appended,
                                                  ProfileItemAcquisition& acquisition) noexcept;

/** Stages one version containing a complete multi-row record reward and claim. */
[[nodiscard]] bool stage_record_reward_grant(const SessionState& before,
                                             std::uint64_t accountSoid,
                                             std::uint64_t characterSoid,
                                             std::span<const std::uint64_t> appendedResidents,
                                             RecordRewardGrant& grant) noexcept;

/** Stages a prepared record-reward grant, appending the residents its new instances need. */
[[nodiscard]] bool stage_record_reward_grant(const SessionState& before,
                                             const state::PendingRecordRewardGrant& pending,
                                             RecordRewardGrant& grant) noexcept;

/**
 * Stages one Family-4 increment that removes an item resident and updates its character.
 *
 * @param before Current active peer state.
 * @param accountSoid Account root the peer's family is rooted with.
 * @param characterSoid Selected resident character losing the item.
 * @param dismantledInstanceSoid Existing item-instance SOID to release.
 * @param updatesAccount True when the account object rides the same increment.
 * @param dismantle Gets the exact +1 version and compacted resident after-image.
 * @return True when both schemas and both named residents exist exactly once.
 */
[[nodiscard]] bool stage_item_dismantle(const SessionState& before,
                                        std::uint64_t accountSoid,
                                        std::uint64_t characterSoid,
                                        std::uint64_t dismantledInstanceSoid,
                                        bool updatesAccount,
                                        ItemDismantle& dismantle) noexcept;

/** Clears one named family. Another family, root or an inactive record changes nothing. */
void stage_unsubscription(const SessionState& before,
                          std::uint32_t familyType,
                          std::uint64_t familyRootSoid,
                          SessionState& after) noexcept;

} // namespace sunrise::server::bap::encrypted::queuez
