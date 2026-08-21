#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/datagen/character_record/character_record_encoder.h"
#include "../../../../middleware/datagen/definitions.h"
#include "../../../../middleware/datagen/family3/family3_roster.h"
#include "../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../middleware/datagen/family4/account/selection_patch/account_selection_patch_encoder.h"
#include "../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/equipment/light/calculation/equipment_light_calculation.h"
#include "../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "../../../../state/runtime/character_creation.h"
#include "../push/queuez/queuez_update_frame.h"
#include "../push/snapshot/internal.h"
#include "../push/snapshot/snapshot_storage.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez::character_creation {
namespace detail {

namespace character_record = ::sunrise::middleware::datagen::character_record;
namespace family4_datagen = ::sunrise::middleware::datagen::family4;
namespace selection_patch =
    ::sunrise::middleware::datagen::family4::account::selection_patch;
namespace snapshot = ::sunrise::server::bap::encrypted::push::snapshot;

namespace queuez_frame = ::sunrise::server::bap::encrypted::push::queuez_frame;

/** Compact creation-publication trace; this is part of the real path, not a diagnostic-only fork. */
inline void report_step(const char* stage,
                        bool ok,
                        const SessionState& state,
                        std::size_t objects = 0,
                        std::size_t written = 0) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=character_create stage=%s result=%s family4=%d family3=%d family0=%d "
        "residents=%u objects=%zu framed=%zu",
        stage,
        ok ? "ok" : "fail",
        state.family4Version,
        state.family3Version,
        state.family0Version,
        static_cast<unsigned>(state.family4ResidentCount),
        objects,
        written);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         ok ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Appends only the item residents introduced by the created character. */
[[nodiscard]] inline bool prepare_family4_items(Scratch& scratch,
                                                const SessionState& before,
                                                const state::PendingCharacterCreation& mutation,
                                                const state::AccountState& account,
                                                snapshot::Prepared& prepared) noexcept {
    if (!valid(before) || !before.family4Active || before.family4RootSoid == 0
        || before.family4RootSoid != account.primarySoid
        || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || !state::account::valid(account) || !mutation.prepared
        || mutation.characterIndex >= account.characterCount
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return false;
    }

    family4_datagen::loadout::ResolvedInstances instances{};
    if (!family4_datagen::loadout::resolve_owned_instances(
            account, mutation.characterIndex, instances)
        || instances.itemCount == 0) {
        return false;
    }

    snapshot::Prepared staged{};
    std::size_t itemCursor = 0;
    std::size_t compressedExtent = 0;
    if (!snapshot::append_items(scratch,
                                std::span(scratch.plaintext),
                                middleware::datagen::kItemInstanceObjectId,
                                instances,
                                0,
                                staged,
                                itemCursor,
                                compressedExtent)
        || itemCursor != instances.itemCount) {
        return false;
    }

    staged.rawClearSize = family4_datagen::instance::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        account.primarySoid,
        before.family4Version + 1,
        0,
        std::span(staged.objects).first(itemCursor),
    };
    return snapshot::commit(staged, prepared);
}

/** Extends the resident manifest with one creation frame without disturbing earlier residents. */
[[nodiscard]] inline bool stage_family4_items(const SessionState& before,
                                              const middleware::queuez::Family& family,
                                              SessionState& after) noexcept {
    after = {};
    if (!valid(before) || !before.family4Active || before.family4RootSoid == 0
        || before.family3Phase != Family3Phase::normal
        || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || family.type != kAccountFamilyType || family.rootSoid != before.family4RootSoid
        || family.version != before.family4Version + 1 || family.flags != 0
        || family.objects.empty()
        || before.family4ResidentCount + family.objects.size() > before.family4Residents.size()) {
        return false;
    }

    SessionState candidate = before;
    for (const middleware::queuez::Object& object : family.objects) {
        if (object.id != middleware::datagen::kItemInstanceObjectId || object.version == 0
            || object.payload.empty()) {
            return false;
        }
        for (std::size_t index = 0; index < candidate.family4ResidentCount; ++index) {
            if (candidate.family4Residents[index].objectSoid == object.version) {
                return false;
            }
        }
        candidate.family4Residents[candidate.family4ResidentCount++] =
            ResidentObject{object.version, object.id};
    }
    candidate.family4Version = family.version;
    if (!valid(candidate)) {
        return false;
    }
    after = candidate;
    return true;
}

/** Builds one incremental Family-3 image containing the roster and every character record. */
[[nodiscard]] inline bool prepare_family3(Scratch& scratch,
                                          const SessionState& before,
                                          const state::AccountState& account,
                                          snapshot::Prepared& prepared) noexcept {
    if (!valid(before) || !before.family3Active || before.family3RootSoid == 0
        || before.family3RootSoid != account.primarySoid
        || before.family3Version == (std::numeric_limits<std::int32_t>::max)()
        || !state::account::valid(account)) {
        return false;
    }

    const auto rawStorage = std::span(scratch.plaintext);
    std::size_t rosterSize = 0;
    if (!middleware::datagen::family3::encode_roster(account, rawStorage, rosterSize)
        || rosterSize == 0 || rosterSize > rawStorage.size()) {
        return false;
    }

    snapshot::Prepared staged{};
    std::size_t compressedExtent = 0;
    std::size_t objectCount = 0;
    if (!snapshot::append_object(scratch,
                                 rawStorage.first(rosterSize),
                                 middleware::datagen::kRosterObjectId,
                                 account.primarySoid,
                                 staged.objects[objectCount++],
                                 compressedExtent)) {
        return false;
    }
    std::size_t rawClearSize = rosterSize;

    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        if (objectCount >= staged.objects.size()
            || character_record::kFamily3RecordSize > rawStorage.size()) {
            return false;
        }
        family4_datagen::loadout::ResolvedInstances instances{};
        std::int32_t light = 0;
        if (!family4_datagen::loadout::resolve_instances(account, characterIndex, instances)
            || !state::equipment::light::resolution::character_light(
                account, characterIndex, light)) {
            return false;
        }
        const auto record = rawStorage.first(character_record::kFamily3RecordSize);
        const state::CharacterState& character = account.characters[characterIndex];
        if (!character_record::encode_family3(character, instances, light, record)
            || !snapshot::append_object(scratch,
                                        record,
                                        middleware::datagen::kRosterCharacterObjectId,
                                        character.soid,
                                        staged.objects[objectCount++],
                                        compressedExtent)) {
            return false;
        }
        rawClearSize = (std::max)(rawClearSize, character_record::kFamily3RecordSize);
    }

    staged.rawClearSize = rawClearSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kRosterFamilyType,
        account.primarySoid,
        before.family3Version + 1,
        0,
        std::span(staged.objects).first(objectCount),
    };
    return snapshot::commit(staged, prepared);
}

/**
 * Rebuilds the selected-character light summary from the same already-resolved native equipment
 * slots the creator publishes. The ordinary resolver also re-validates cross-character authored
 * slot metadata; creation has already passed loadout::resolve(), so repeating that stricter pass
 * here can reject an otherwise canonical starter loadout before its first character object exists.
 */
[[nodiscard]] inline bool resolve_creation_light(
    const state::AccountState& account,
    std::size_t selectedIndex,
    state::equipment::light::Evaluation& output) noexcept {
    namespace light = ::sunrise::state::equipment::light;
    namespace calculation = ::sunrise::state::equipment::light::calculation;

    if (!state::account::valid(account) || selectedIndex >= account.characterCount
        || !account.characters[selectedIndex].selected) {
        return false;
    }

    std::array<light::SlotScores, state::kCharacterCapacity> scores{};
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        family4_datagen::loadout::ResolvedInstances instances{};
        if (!family4_datagen::loadout::resolve_instances(account, characterIndex, instances)) {
            return false;
        }
        light::SlotScores& characterScores = scores[characterIndex];
        for (std::size_t itemIndex = 0; itemIndex < instances.itemCount; ++itemIndex) {
            const auto& item = instances.items[itemIndex];
            const std::size_t slot = static_cast<std::size_t>(item.equipmentSlot);
            const std::int32_t level = item.instance.level;
            if (slot >= characterScores.size() || characterScores[slot].has_value()
                || level < 0
                || (level > 0
                    && level > (std::numeric_limits<std::int32_t>::max)()
                                   / light::kPowerPerLevel)) {
                return false;
            }
            const std::int32_t rawScore = level > 0 ? level * light::kPowerPerLevel : 0;
            const std::int32_t score =
                level > 0 ? (std::max)(light::kMinimumItemPower, rawScore) : 0;
            characterScores[slot] = light::ItemScore{
                item.instance.baseDefinitionIndex,
                score,
            };
        }
    }

    std::array<light::SlotScores, state::kCharacterCapacity - 1U> others{};
    std::size_t otherCount = 0;
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        if (characterIndex == selectedIndex) {
            continue;
        }
        if (otherCount >= others.size()) {
            return false;
        }
        others[otherCount++] = scores[characterIndex];
    }

    // Current Sunrise uses the selected character's equipped maxima for the profile summary too;
    // preserve that policy while allowing the other characters to contribute strict power upgrades
    // to the aggregate total/light value.
    return calculation::evaluate(scores[selectedIndex],
                                 scores[selectedIndex],
                                 std::span(others).first(otherCount),
                                 output);
}

/** Builds the normal Family-4 character-selection move from the uncommitted account after-image. */
[[nodiscard]] inline bool prepare_selection_move(Scratch& scratch,
                                                 const SelectCharacter& select,
                                                 const state::AccountState& account,
                                                 snapshot::Prepared& prepared) noexcept {
    if (!valid(select.after) || !state::account::valid(account)
        || select.selectedCharacterSoid == 0
        || account.primarySoid != select.after.family4RootSoid
        || state::account::selected_character_soid(account) != select.selectedCharacterSoid) {
        report_step("select_prepare_input", false, select.after);
        return false;
    }

    std::size_t selectedIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == select.selectedCharacterSoid) {
            selectedIndex = index;
            break;
        }
    }
    family4_datagen::loadout::ResolvedLoadout selectedLoadout{};
    state::equipment::light::Evaluation selectedLight{};
    if (selectedIndex >= account.characterCount) {
        report_step("select_character_index", false, select.after);
        return false;
    }
    if (!family4_datagen::loadout::resolve(account, selectedIndex, selectedLoadout)) {
        report_step("select_loadout", false, select.after);
        return false;
    }
    if (!resolve_creation_light(account, selectedIndex, selectedLight)) {
        report_step("select_light", false, select.after);
        return false;
    }

    const auto rawStorage = std::span(scratch.plaintext);
    snapshot::Prepared staged{};
    std::size_t compressedExtent = 0;
    std::size_t objectCount = 0;
    if (select.previousCharacterSoid != 0) {
        staged.objects[objectCount++] = middleware::queuez::Object{
            select.characterDefinitionId,
            select.previousCharacterSoid,
            middleware::queuez::Encoding::oodle,
            {},
        };
    }

    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        report_step("select_character_storage", false, select.after);
        return false;
    }
    const auto characterBytes =
        rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[selectedIndex],
                                            selectedLoadout,
                                            selectedLight,
                                            characterBytes)) {
        report_step("select_character_encode", false, select.after);
        return false;
    }
    if (!snapshot::append_object(scratch,
                                 characterBytes,
                                 select.characterDefinitionId,
                                 select.selectedCharacterSoid,
                                 staged.objects[objectCount++],
                                 compressedExtent)) {
        report_step("select_character_object", false, select.after);
        return false;
    }
    staged.rawClearSize = family4_datagen::character::layout::kObjectSize;

    if (select.patchAccount) {
        std::size_t patchSize = 0;
        if (!selection_patch::encode(select.selectedCharacterSoid, rawStorage, patchSize)
            || patchSize != selection_patch::kPayloadSize
            || objectCount >= staged.objects.size()) {
            report_step("select_account_patch", false, select.after);
            return false;
        }
        staged.objects[objectCount++] = middleware::queuez::Object{
            select.accountDefinitionId,
            select.after.family4RootSoid,
            middleware::queuez::Encoding::tagReflection,
            rawStorage.first(patchSize),
        };
        staged.rawClearSize = (std::max)(staged.rawClearSize, patchSize);
        report_step("select_account_patch", true, select.after, objectCount);
    } else {
        if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
            report_step("select_account_full_storage", false, select.after);
            return false;
        }
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        if (!family4_datagen::account::encode(account, accountBytes)
            || !snapshot::append_object(scratch,
                                        accountBytes,
                                        select.accountDefinitionId,
                                        select.after.family4RootSoid,
                                        staged.objects[objectCount++],
                                        compressedExtent)) {
            report_step("select_account_full", false, select.after);
            return false;
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize, family4_datagen::account::layout::kObjectSize);
        report_step("select_account_full", true, select.after, objectCount);
    }

    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        select.after.family4RootSoid,
        select.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    const bool committed = snapshot::commit(staged, prepared);
    if (!committed) {
        report_step("select_commit", false, select.after, objectCount);
    }
    return committed;
}

/** Builds the Family-0 move to a newly created selected character from the same after-image. */
[[nodiscard]] inline bool prepare_family0(Scratch& scratch,
                                          const SessionState& before,
                                          const state::AccountState& account,
                                          snapshot::Prepared& prepared,
                                          SessionState& after) noexcept {
    after = before;
    const std::uint64_t selected = state::account::selected_character_soid(account);
    bool publish = false;
    bool incremental = false;
    if (selected == 0 || !before.family0Active
        || !stage_family0_subscription(before, selected, publish, incremental, after)
        || before.family4RootSoid != account.primarySoid) {
        return false;
    }

    // Deletion can free a character SOID that creation immediately reuses. Family zero may still
    // own that key even though its resident body belongs to the deleted Guardian. Mirror the
    // ordinary character-pick path: selecting the same key republishes it in place at the next
    // Family-0 version instead of treating the no-move result as a failure.
    if (!publish) {
        if (before.family0Character != selected
            || before.family0Version == (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        incremental = false;
        after = before;
        ++after.family0Version;
        if (!valid(after)) {
            return false;
        }
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == selected) {
            characterIndex = index;
            break;
        }
    }
    family4_datagen::loadout::ResolvedInstances instances{};
    std::int32_t light = 0;
    if (characterIndex >= account.characterCount
        || !family4_datagen::loadout::resolve_instances(account, characterIndex, instances)
        || !state::equipment::light::resolution::character_light(account, characterIndex, light)) {
        return false;
    }

    constexpr std::size_t kRawSize =
        character_record::kFamily0AnchorSize + character_record::kFamily0RecordSize;
    const auto rawStorage = std::span(scratch.plaintext);
    if (kRawSize > rawStorage.size()) {
        return false;
    }
    const auto anchor = rawStorage.first(character_record::kFamily0AnchorSize);
    const auto record = rawStorage.subspan(character_record::kFamily0AnchorSize,
                                           character_record::kFamily0RecordSize);
    const state::CharacterState& character = account.characters[characterIndex];
    if (!character_record::encode_family0_anchor(account.primarySoid, character.soid, anchor)
        || !character_record::encode_family0(character, instances, light, record)) {
        return false;
    }

    snapshot::Prepared staged{};
    std::size_t objectCount = 0;
    if (incremental && before.family0Character != 0) {
        staged.objects[objectCount++] = middleware::queuez::Object{
            middleware::datagen::kBannerCharacterObjectId,
            before.family0Character,
            middleware::queuez::Encoding::raw,
            {},
        };
    }
    std::size_t compressedExtent = 0;
    if (!snapshot::append_object(scratch,
                                 anchor,
                                 middleware::datagen::kBannerAnchorObjectId,
                                 account.primarySoid,
                                 staged.objects[objectCount++],
                                 compressedExtent)
        || !snapshot::append_object(scratch,
                                    record,
                                    middleware::datagen::kBannerCharacterObjectId,
                                    character.soid,
                                    staged.objects[objectCount++],
                                    compressedExtent)) {
        return false;
    }
    staged.rawClearSize = kRawSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kBannerFamilyType,
        account.primarySoid,
        after.family0Version,
        middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    return snapshot::commit(staged, prepared);
}

} // namespace detail

/**
 * Appends every QueueZ after-image required by one prepared character creation.
 *
 * The measured order is preserved: new item residents first, then the complete Family-3 roster.
 * Existing rosters then take the ordinary Family-4 character-selection move and Family-0 banner
 * move. The first real Guardian stops after the roster and remains unselected.
 */
[[nodiscard]] inline bool append(Scratch& scratch,
                                 const SessionState& before,
                                 const state::PendingCharacterCreation& mutation,
                                 const state::AccountState& accountAfter,
                                 std::span<const std::byte, state::kAesKeySize> key,
                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                 std::span<std::byte> response,
                                 std::size_t& written,
                                 SessionState& after) noexcept {
    after = before;
    const bool inputValid =
        mutation.prepared && mutation.accountSoid != 0 && mutation.characterSoid != 0
        && mutation.accountSoid == accountAfter.primarySoid
        && mutation.characterIndex < accountAfter.characterCount
        && accountAfter.characters[mutation.characterIndex].soid == mutation.characterSoid
        && mutation.selectCreated
               == (state::account::selected_character_soid(accountAfter)
                   == mutation.characterSoid)
        && valid(before) && before.family4Active && before.family3Active
        && before.family4RootSoid == mutation.accountSoid
        && before.family3RootSoid == mutation.accountSoid
        && before.family3Phase == Family3Phase::normal
        && (!mutation.selectCreated || before.family0Active);
    if (!inputValid) {
        detail::report_step("publication_input", false, before, 0, written);
        return false;
    }

    SessionState current = before;

    // 1) Publish only the new Guardian's item-instance residents. The live account object and
    // existing resident manifest stay untouched.
    detail::snapshot::Prepared family4Items{};
    if (!detail::prepare_family4_items(
            scratch, current, mutation, accountAfter, family4Items)) {
        detail::report_step("family4_items_prepare", false, current, 0, written);
        return false;
    }
    detail::report_step(
        "family4_items_prepare", true, current, family4Items.family.objects.size(), written);

    SessionState family4After{};
    if (!detail::stage_family4_items(current, family4Items.family, family4After)) {
        detail::report_step(
            "family4_items_stage", false, current, family4Items.family.objects.size(), written);
        return false;
    }
    if (!detail::queuez_frame::append(scratch,
                                      family4Items.family,
                                      family4Items.rawClearSize,
                                      family4Items.compressedClearSize,
                                      key,
                                      nonce,
                                      response,
                                      written)) {
        detail::report_step(
            "family4_items_frame", false, current, family4Items.family.objects.size(), written);
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    current = family4After;
    detail::report_step(
        "family4_items", true, current, family4Items.family.objects.size(), written);

    // 2) Publish the complete roster after-image at the next Family-3 version. This is the same
    // object order as the normal full roster: account roster first, then one record per Guardian.
    detail::snapshot::Prepared family3{};
    if (!detail::prepare_family3(scratch, current, accountAfter, family3)) {
        detail::report_step("family3_prepare", false, current, 0, written);
        return false;
    }
    SessionState family3After = current;
    if (family3After.family3Version == (std::numeric_limits<std::int32_t>::max)()) {
        detail::report_step(
            "family3_version", false, current, family3.family.objects.size(), written);
        return false;
    }
    ++family3After.family3Version;
    // Creation changes the account roster itself, so keep the normal Family-3 phase here.
    // stage_select_character() will therefore use the measured first-pick path and publish the
    // full Family-4 account after-image, including the newly created character. The compact
    // selection patch is reserved for the opcode-505 preselection flow, where the roster did not
    // change and resending the account body would wipe resident settings state.
    if (!valid(family3After) || family3.family.version != family3After.family3Version
        || family3.family.rootSoid != family3After.family3RootSoid) {
        detail::report_step(
            "family3_stage", false, current, family3.family.objects.size(), written);
        return false;
    }
    if (!detail::queuez_frame::append(scratch,
                                      family3.family,
                                      family3.rawClearSize,
                                      family3.compressedClearSize,
                                      key,
                                      nonce,
                                      response,
                                      written)) {
        detail::report_step(
            "family3_frame", false, current, family3.family.objects.size(), written);
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    current = family3After;
    detail::report_step("family3", true, current, family3.family.objects.size(), written);

    // The first real Guardian is intentionally left unselected. Existing rosters continue through
    // the ordinary selection/banner transition below.
    if (!mutation.selectCreated) {
        after = current;
        detail::report_step("publication_complete", valid(after), after, 0, written);
        return valid(after);
    }

    // 3) Add/move the single Family-4 character resident with the same stage contract used by a
    // normal character pick. The custom preparer only supplies the uncommitted account after-image.
    SelectCharacter select{};
    if (!stage_select_character(current, mutation.characterSoid, select)) {
        detail::report_step("select_stage", false, current, 0, written);
        return false;
    }
    detail::snapshot::Prepared selection{};
    if (!detail::prepare_selection_move(scratch, select, accountAfter, selection)) {
        detail::report_step("select_prepare", false, current, 0, written);
        return false;
    }
    if (!detail::queuez_frame::append(scratch,
                                      selection.family,
                                      selection.rawClearSize,
                                      selection.compressedClearSize,
                                      key,
                                      nonce,
                                      response,
                                      written)) {
        detail::report_step(
            "select_frame", false, current, selection.family.objects.size(), written);
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    current = select.after;
    detail::report_step("select", true, current, selection.family.objects.size(), written);

    // 4) Move the Family-0 banner pair to the newly selected Guardian.
    detail::snapshot::Prepared family0{};
    SessionState family0After{};
    if (!detail::prepare_family0(scratch, current, accountAfter, family0, family0After)) {
        detail::report_step("family0_prepare", false, current, 0, written);
        return false;
    }
    if (!detail::queuez_frame::append(scratch,
                                      family0.family,
                                      family0.rawClearSize,
                                      family0.compressedClearSize,
                                      key,
                                      nonce,
                                      response,
                                      written)) {
        detail::report_step(
            "family0_frame", false, current, family0.family.objects.size(), written);
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    current = family0After;
    detail::report_step("family0", true, current, family0.family.objects.size(), written);

    after = current;
    const bool complete = valid(after);
    detail::report_step("publication_complete", complete, after, 0, written);
    return complete;
}

} // namespace sunrise::server::bap::encrypted::queuez::character_creation
