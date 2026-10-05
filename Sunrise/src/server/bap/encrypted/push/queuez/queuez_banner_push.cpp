#include <array>
#include <cstdio>
#include <limits>
#include <span>

#include "../../../../../core/logging/log.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/snapshot.h"
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
    // Runs before the account is read, so this pair matches the family-three and family-four
    // images.
    ensure_account_canonical();
    // The pair names the first character when none is picked yet. The client's family-zero record
    // accepts a snapshot for about ten seconds, then clears the family and refuses every later
    // one, so holding the pair for the pick spends that window and the subscription times out.
    if (state::account::banner_character_soid(state::bound_account_snapshot()) == 0) {
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
    if (!queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner result=fail reason=frame");
        return false;
    }
    // The Client now holds this pair, so the ladder owns it. Without this an unsubscribe leaves
    // family zero unrecorded and the next pick has no previous record to release.
    const std::uint64_t delivered =
        state::account::banner_character_soid(state::bound_account_snapshot());
    if (!after.family0Active && delivered != 0) {
        after.family0Active = true;
        after.family0Character = delivered;
        after.family0Version = queuez::kInitialFamilyVersion;
    }
    return true;
}

/**
 * Appends the family-zero pair that follows an opcode-504 pick. The Client holds the objIdx-1
 * buffer for one character at a time, so the pair moves with the pick or the banner keeps the old
 * emblem. A pick on the character it already holds republishes in place.
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
        if (before.family0Version == (std::numeric_limits<std::int32_t>::max)()) {
            report_skip("version_exhausted");
            after = before;
            return false;
        }
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
    if (!queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written)) {
        report_fail(stage, "frame");
        after = before;
        return false;
    }
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
    return queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written);
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
    return queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written);
}

/** Appends one Family-0 character ability refresh after a subclass selection. */
bool append_subclass_appearance_refresh_notification(
    Scratch& scratch,
    const queuez::CharacterAppearanceRefresh& refresh,
    const state::PendingSubclassSelection& mutation,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    // Index of the subclass slot in the authored equipment array.
    constexpr std::size_t kSubclassSlot =
        static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);
    if (!mutation.prepared || mutation.characterSoid != refresh.characterSoid
        || kSubclassSlot >= mutation.afterCharacter.equipment.slots.size()) {
        return false;
    }
    const auto& subclass = mutation.afterCharacter.equipment.slots[kSubclassSlot];
    if (!subclass.has_value() || subclass->instanceSoid != mutation.subclassInstanceSoid) {
        return false;
    }
    state::build_data::items::details::Definition detail{};
    if (!state::build_data::find_configured_item_detail(mutation.subclassDefinitionIndex, detail)
        || detail.definitionIndex != mutation.subclassDefinitionIndex
        || detail.definitionHash != mutation.subclassDefinitionHash
        || !detail.equipmentSlot.has_value() || *detail.equipmentSlot < 0
        || static_cast<std::size_t>(*detail.equipmentSlot)
               >= state::build_data::items::details::kEquipmentSlotCount) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_character_appearance_refresh(
            scratch,
            refresh,
            mutation.afterCharacter,
            mutation.characterIndex,
            static_cast<std::uint8_t>(*detail.equipmentSlot),
            true,
            prepared)) {
        return false;
    }
    return queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written);
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
    return queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written);
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
        || mutation.itemIndex >= mutation.afterCharacter.equipment.slots.size()) {
        return false;
    }
    const auto& target = mutation.afterCharacter.equipment.slots[mutation.itemIndex];
    if (!target.has_value() || target->instanceSoid != mutation.targetInstanceSoid) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_roster_appearance_refresh(
            scratch, refresh, mutation.afterCharacter, mutation.characterIndex, prepared)) {
        return false;
    }
    return queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written);
}

/** Appends the Family-3 character-only refresh owed by a subclass selection. */
bool append_subclass_roster_refresh_notification(Scratch& scratch,
                                                 const queuez::RosterAppearanceRefresh& refresh,
                                                 const state::PendingSubclassSelection& mutation,
                                                 std::span<const std::byte, state::kAesKeySize> key,
                                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                                 std::span<std::byte> response,
                                                 std::size_t& written) noexcept {
    // Index of the subclass slot in the authored equipment array.
    constexpr std::size_t kSubclassSlot =
        static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);
    if (!mutation.prepared || refresh.includeRoster
        || mutation.characterSoid != refresh.characterSoid
        || kSubclassSlot >= mutation.afterCharacter.equipment.slots.size()) {
        return false;
    }
    const auto& subclass = mutation.afterCharacter.equipment.slots[kSubclassSlot];
    if (!subclass.has_value() || subclass->instanceSoid != mutation.subclassInstanceSoid) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_roster_appearance_refresh(
            scratch, refresh, mutation.afterCharacter, mutation.characterIndex, prepared)) {
        return false;
    }
    return queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written);
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
    const state::AccountState account = state::bound_account_snapshot();
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
        || !queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written)) {
        return false;
    }
    after = refresh.after;
    return true;
}

/** Refreshes the selected character and includes its roster only when roster fields changed. */
bool append_account_resync_roster_notification(Scratch& scratch,
                                               const queuez::SessionState& before,
                                               std::span<const std::byte, state::kAesKeySize> key,
                                               std::array<std::byte, state::kBapNonceSize>& nonce,
                                               std::span<std::byte> response,
                                               std::size_t& written,
                                               queuez::SessionState& after,
                                               bool includeRoster) noexcept {
    after = before;
    ensure_account_canonical();
    if (!before.family3Active) {
        return true;
    }
    const state::AccountState account = state::bound_account_snapshot();
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
        || !queuez::stage_roster_appearance_refresh(before, selected, includeRoster, refresh)
        || !snapshot::prepare_roster_appearance_refresh(
            scratch, refresh, account.characters[characterIndex], characterIndex, prepared)
        || !queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written)) {
        return false;
    }
    after = refresh.after;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push
