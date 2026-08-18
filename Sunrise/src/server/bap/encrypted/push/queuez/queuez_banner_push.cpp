#include <array>
#include <cstdio>
#include <span>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/snapshot.h"
#include "queuez_push_reporting.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {
namespace {

/** One line carries the refusal key and nothing else. */
constexpr std::size_t kSkipLineCapacity = 96;

/**
 * Reports the banner move declining to build a frame.
 * Both refusals leave the emblem where it is, so the key is the only way to tell a ladder that was
 * never seeded from one with no root.
 * @param reason Key naming the refusal.
 */
void report_skip(const char* reason) noexcept {
    std::array<char, kSkipLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=queuez stage=banner_move result=skip reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Reports the banner move or its republish failing to build a frame.
 * @param stage Stage name the caller resolved, so a republish does not report as a move.
 * @param reason Key naming the failure.
 */
void report_fail(const char* stage, const char* reason) noexcept {
    std::array<char, kSkipLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=queuez stage=%s result=fail reason=%s", stage, reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Validates and appends one same-character Family-0 appearance refresh. */
[[nodiscard]] bool append_appearance_frame(Scratch& scratch,
                                           const queuez::CharacterAppearanceRefresh& refresh,
                                           snapshot::Prepared& prepared,
                                           const char* stage,
                                           std::span<const std::byte, state::kAesKeySize> key,
                                           std::array<std::byte, state::kBapNonceSize>& nonce,
                                           std::span<std::byte> response,
                                           std::size_t& written) noexcept {
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    const bool replacement =
        objectCount >= 2
        && prepared.family.objects.front().id == middleware::datagen::kBannerCharacterObjectId
        && prepared.family.objects.front().version == refresh.characterSoid
        && prepared.family.objects.front().payload.empty();
    const std::size_t characterIndex = replacement ? 1U : 0U;
    const bool hasAnchor = objectCount == characterIndex + 2U;
    if ((objectCount != characterIndex + 1U && !hasAnchor)
        || prepared.family.type != queuez::kBannerFamilyType
        || prepared.family.rootSoid != refresh.after.family4RootSoid
        || prepared.family.version != refresh.after.family0Version || prepared.family.flags != 0
        || (replacement
            && prepared.family.objects.front().encoding != middleware::queuez::Encoding::raw)
        || prepared.family.objects[characterIndex].id
               != middleware::datagen::kBannerCharacterObjectId
        || prepared.family.objects[characterIndex].version != refresh.characterSoid
        || prepared.family.objects[characterIndex].encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects[characterIndex].payload.empty()
        || (hasAnchor
            && (prepared.family.objects.back().id != middleware::datagen::kBannerAnchorObjectId
                || prepared.family.objects.back().version != refresh.after.family4RootSoid
                || prepared.family.objects.back().encoding != middleware::queuez::Encoding::oodle
                || prepared.family.objects.back().payload.empty()))
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    queuez_report::push(stage, queuez::kBannerFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

/** Validates and appends one incremental Family-3 appearance frame. */
[[nodiscard]] bool
append_roster_appearance_frame(Scratch& scratch,
                               const queuez::RosterAppearanceRefresh& refresh,
                               snapshot::Prepared& prepared,
                               const char* stage,
                               std::span<const std::byte, state::kAesKeySize> key,
                               std::array<std::byte, state::kBapNonceSize>& nonce,
                               std::span<std::byte> response,
                               std::size_t& written) noexcept {
    const std::size_t expectedObjects = refresh.includeRoster ? 2U : 1U;
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (objectCount != expectedObjects || prepared.family.type != queuez::kRosterFamilyType
        || prepared.family.rootSoid != refresh.after.family3RootSoid
        || prepared.family.version != refresh.after.family3Version || prepared.family.flags != 0
        || prepared.family.objects.front().id != middleware::datagen::kRosterCharacterObjectId
        || prepared.family.objects.front().version != refresh.characterSoid
        || prepared.family.objects.front().encoding != middleware::queuez::Encoding::oodle
        || prepared.family.objects.front().payload.empty()
        || (refresh.includeRoster
            && (prepared.family.objects.back().id != middleware::datagen::kRosterObjectId
                || prepared.family.objects.back().version != refresh.after.family3RootSoid
                || prepared.family.objects.back().encoding != middleware::queuez::Encoding::oodle
                || prepared.family.objects.back().payload.empty()))
        || !queuez_frame::append(scratch,
                                 prepared.family,
                                 prepared.rawClearSize,
                                 prepared.compressedClearSize,
                                 key,
                                 nonce,
                                 response,
                                 written)) {
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    queuez_report::push(stage, queuez::kRosterFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

} // namespace

/**
 * Appends the unsolicited family-zero banner pair that follows a family-three subscription.
 * Sent twice per boot at the same version: family zero hits the state-1 race with no svc-12
 * re-push behind it, so a rejected first pair would blank the banner for the run.
 * @param scratch Lock-owned transform buffers.
 * @param familyRootSoid Root the Client subscribed for Family 3.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced only by a complete frame.
 * @param response Caller-owned output containing prior frames.
 * @param written Existing byte count, updated by a complete frame.
 * @return True when the banner frame is appended.
 */
bool append_banner_notification(Scratch& scratch,
                                const queuez::SessionState& before,
                                std::uint64_t familyRootSoid,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                queuez::SessionState& after) noexcept {
    after = before;
    // Before the account is read, so this pair cannot describe a different account than the
    // family-three roster or the family-four manifest.
    ensure_account_canonical();
    // The pair names the first character when none is picked yet. The client's family-zero record
    // accepts a snapshot for about ten seconds, then clears the family and refuses every later
    // one, so holding the pair for the pick spends that window and the subscription times out.
    if (state::account::banner_character_soid(state::account_snapshot()) == 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=banner result=skip reason=nocharacter");
        return false;
    }
    snapshot::Prepared prepared{};
    // The unsolicited pair is the family's first delivery, so it carries the full-snapshot flag.
    if (!snapshot::prepare_banner(
            scratch, familyRootSoid, queuez::kInitialFamilyVersion, 0, prepared)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner result=fail reason=prepare");
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner result=fail reason=frame");
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    // The Client now holds this pair, so the ladder owns it. Without this an unsubscribe leaves
    // family zero unrecorded and the next pick has no previous record to release.
    const std::uint64_t delivered =
        state::account::banner_character_soid(state::account_snapshot());
    if (!after.family0Active && delivered != 0) {
        after.family0Active = true;
        after.family0Character = delivered;
        after.family0Version = queuez::kInitialFamilyVersion;
    }
    queuez_report::push("banner",
                        prepared.family.type,
                        objectCount,
                        written - beforeBytes,
                        queuez_report::kNoRecordOutcome);
    return true;
}

/**
 * Appends the family-zero pair that follows an opcode-504 pick.
 * The Client holds the objIdx-1 buffer for one character at a time, allocated from the character
 * the anchor names, so the pair moves with the pick or the banner keeps the old emblem. A pick
 * naming the character the pair already holds republishes it in place.
 * @param scratch Lock-owned transform buffers.
 * @param before Queuez state after the family-four move.
 * @param selectedCharacter Character the pick named.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced only by a complete frame.
 * @param response Caller-owned output containing prior frames.
 * @param written Existing byte count, updated by a complete frame.
 * @param after Receives the state published once the frame is copied.
 * @return True when a frame went out and `after` carries the advanced ladder.
 */
bool append_banner_move_notification(Scratch& scratch,
                                     const queuez::SessionState& before,
                                     std::uint64_t selectedCharacter,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::array<std::byte, state::kBapNonceSize>& nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written,
                                     queuez::SessionState& after) noexcept {
    bool publish = false;
    bool incremental = false;
    after = before;
    ensure_account_canonical();
    // A family zero with no first delivery yet has no ladder to move, and no root to name it with.
    const char* reason = nullptr;
    if (!queuez::stage_family0_subscription(
            before, selectedCharacter, publish, incremental, after)) {
        reason = "stage";
    } else if (before.family4RootSoid == 0) {
        reason = "no_root";
    }
    if (reason != nullptr) {
        report_skip(reason);
        after = before;
        return false;
    }
    // A pick naming the character the pair already holds still republishes it: the only body the
    // Client would otherwise hold is the boot burst's, built before any pick. Nothing is released,
    // because deleting the key the same frame re-adds tears the family down.
    const bool republish = !publish;
    if (republish) {
        incremental = false;
        after.family0Version = before.family0Version + 1;
    }
    const char* const stage = republish ? "banner_republish" : "banner_move";
    snapshot::Prepared prepared{};
    // A first delivery releases nothing: the Client holds no record for this family yet, so the
    // pair goes out as its own full snapshot instead of as a move off a previous character.
    if (!snapshot::prepare_banner(scratch,
                                  before.family4RootSoid,
                                  after.family0Version,
                                  incremental ? before.family0Character : 0,
                                  prepared)) {
        report_fail(stage, "prepare");
        after = before;
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        report_fail(stage, "frame");
        after = before;
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    queuez_report::push(stage,
                        prepared.family.type,
                        objectCount,
                        written - beforeBytes,
                        queuez_report::kNoRecordOutcome);
    return true;
}

/** Appends one same-character Family-0 appearance-record upsert after an equipment swap. */
bool append_equipment_appearance_refresh_notification(
    Scratch& scratch,
    const queuez::CharacterAppearanceRefresh& refresh,
    const state::PendingEquipmentSwap& mutation,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    if (!mutation.prepared || mutation.characterSoid != refresh.characterSoid) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_character_appearance_refresh(scratch,
                                                        refresh,
                                                        mutation.afterCharacter,
                                                        mutation.characterIndex,
                                                        mutation.nativeEquipmentSlot,
                                                        false,
                                                        prepared)) {
        return false;
    }
    return append_appearance_frame(
        scratch, refresh, prepared, "equip_appearance", key, nonce, response, written);
}

/** Appends one Family-0 record upsert after a socket change on an equipped item. */
bool append_socket_appearance_refresh_notification(
    Scratch& scratch,
    const queuez::CharacterAppearanceRefresh& refresh,
    const state::PendingSocketPlug& mutation,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    if (!mutation.prepared || !mutation.targetEquipped
        || mutation.characterSoid != refresh.characterSoid
        || mutation.itemIndex >= mutation.afterCharacter.equipment.slots.size()
        || !mutation.afterCharacter.equipment.slots[mutation.itemIndex].has_value()) {
        return false;
    }
    const state::account::inventory::Item& target =
        *mutation.afterCharacter.equipment.slots[mutation.itemIndex];
    if (target.instanceSoid != mutation.targetInstanceSoid) {
        return false;
    }
    state::build_data::items::details::Definition detail{};
    std::uint8_t nativeEquipmentSlot = 0;
    if (!state::build_data::find_configured_item_detail(mutation.targetDefinitionIndex, detail)
        || detail.definitionIndex != mutation.targetDefinitionIndex
        || detail.definitionHash != mutation.targetDefinitionHash
        || detail.bucketId != mutation.targetBucketId
        || !state::account::inventory::resolve_native_equipment_slot(
            mutation.targetDefinitionHash, detail.equipmentSlot, nativeEquipmentSlot)
        || static_cast<std::size_t>(nativeEquipmentSlot)
               >= state::build_data::items::details::kEquipmentSlotCount) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_character_appearance_refresh(scratch,
                                                        refresh,
                                                        mutation.afterCharacter,
                                                        mutation.characterIndex,
                                                        nativeEquipmentSlot,
                                                        true,
                                                        prepared)) {
        return false;
    }
    return append_appearance_frame(
        scratch, refresh, prepared, "socket_appearance", key, nonce, response, written);
}

/** Appends the Family-3 character-then-roster refresh owed by one equipment mutation. */
bool append_equipment_roster_refresh_notification(
    Scratch& scratch,
    const queuez::RosterAppearanceRefresh& refresh,
    const state::PendingEquipmentSwap& mutation,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    if (!mutation.prepared || !refresh.includeRoster
        || mutation.characterSoid != refresh.characterSoid) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_roster_appearance_refresh(
            scratch, refresh, mutation.afterCharacter, mutation.characterIndex, prepared)) {
        return false;
    }
    return append_roster_appearance_frame(
        scratch, refresh, prepared, "equip_roster", key, nonce, response, written);
}

/** Appends the Family-3 character-only refresh owed by a socket change on equipped gear. */
bool append_socket_roster_refresh_notification(Scratch& scratch,
                                               const queuez::RosterAppearanceRefresh& refresh,
                                               const state::PendingSocketPlug& mutation,
                                               std::span<const std::byte, state::kAesKeySize> key,
                                               std::array<std::byte, state::kBapNonceSize>& nonce,
                                               std::span<std::byte> response,
                                               std::size_t& written) noexcept {
    if (!mutation.prepared || !mutation.targetEquipped || refresh.includeRoster
        || mutation.characterSoid != refresh.characterSoid
        || mutation.itemIndex >= mutation.afterCharacter.equipment.slots.size()
        || !mutation.afterCharacter.equipment.slots[mutation.itemIndex].has_value()
        || mutation.afterCharacter.equipment.slots[mutation.itemIndex]->instanceSoid
               != mutation.targetInstanceSoid) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_roster_appearance_refresh(
            scratch, refresh, mutation.afterCharacter, mutation.characterIndex, prepared)) {
        return false;
    }
    return append_roster_appearance_frame(
        scratch, refresh, prepared, "socket_roster", key, nonce, response, written);
}

/** Refreshes the selected character's complete Family-0 appearance from committed State. */
bool append_account_resync_appearance_notification(
    Scratch& scratch,
    const queuez::SessionState& before,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written,
    queuez::SessionState& after) noexcept {
    after = before;
    ensure_account_canonical();
    if (!before.family0Active) {
        return true;
    }
    const state::AccountState account = state::account_snapshot();
    const std::uint64_t selected = state::account::selected_character_soid(account);
    if (selected == 0) {
        return false;
    }
    if (before.family0Character != selected) {
        return append_banner_move_notification(
            scratch, before, selected, key, nonce, response, written, after);
    }
    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == selected) {
            characterIndex = index;
            break;
        }
    }
    queuez::CharacterAppearanceRefresh refresh{};
    snapshot::Prepared prepared{};
    if (characterIndex >= account.characterCount
        || !queuez::stage_character_appearance_refresh(before, selected, refresh)
        || !snapshot::prepare_character_appearance_refresh(
            scratch, refresh, account.characters[characterIndex], characterIndex, 0, true, prepared)
        || !append_appearance_frame(
            scratch, refresh, prepared, "peer_resync_appearance", key, nonce, response, written)) {
        return false;
    }
    after = refresh.after;
    return true;
}

/** Refreshes the selected character and its account roster from committed State. */
bool append_account_resync_roster_notification(Scratch& scratch,
                                               const queuez::SessionState& before,
                                               std::span<const std::byte, state::kAesKeySize> key,
                                               std::array<std::byte, state::kBapNonceSize>& nonce,
                                               std::span<std::byte> response,
                                               std::size_t& written,
                                               queuez::SessionState& after) noexcept {
    after = before;
    ensure_account_canonical();
    if (!before.family3Active) {
        return true;
    }
    const state::AccountState account = state::account_snapshot();
    const std::uint64_t selected = state::account::selected_character_soid(account);
    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == selected) {
            characterIndex = index;
            break;
        }
    }
    queuez::RosterAppearanceRefresh refresh{};
    snapshot::Prepared prepared{};
    if (selected == 0 || characterIndex >= account.characterCount
        || !queuez::stage_roster_appearance_refresh(before, selected, true, refresh)
        || !snapshot::prepare_roster_appearance_refresh(
            scratch, refresh, account.characters[characterIndex], characterIndex, prepared)
        || !append_roster_appearance_frame(
            scratch, refresh, prepared, "peer_resync_roster", key, nonce, response, written)) {
        return false;
    }
    after = refresh.after;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push
