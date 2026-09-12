#include "character_encoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>

#include "../../../../state/build_data/runtime.h"
#include "../../../../state/unlocks/unlocks_records.h"
#include "../../../../state/unlocks/unlocks_runtime.h"
#include "../../character_record/layout.h"
#include "../instance/layout.h"
#include "../progression/progression_bank_keys.h"
#include "equipment_summary_builder.h"
#include "layout.h"

namespace sunrise::middleware::datagen::family4::character {
namespace {

/** Every bit set is the native empty biased 16-bit definition index. */
constexpr std::uint16_t kEmptyDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
/** Every bit set is the native absent activity index. 0 is the orbit activity, not absent. */
constexpr std::uint16_t kAbsentActivityIndex = (std::numeric_limits<std::uint16_t>::max)();
/** Every set bit marks all default per-character messages as already seen. */
constexpr std::byte kSeenMessageByte{0xFF};
/** Native stack insertion treats -1 as an unused row; 0 is a real selector. */
constexpr std::int16_t kEmptyItemStackSelector = -1;
/** Native 1-byte booleans encode true as 1. */
constexpr std::uint8_t kNativeTrue = 1;
/** Native 1-byte booleans encode false as 0. */
constexpr std::uint8_t kNativeFalse = 0;
/** Stackable quest items needed by the collectible interactions currently supported. */
struct CollectibleQuest {
    std::uint32_t definitionHash{};
    /** Lore completion flag which consumes this quest; zero keeps the prerequisite authored. */
    std::uint16_t completionFlag{};
};
/** One row per supported collectible quest; a claimed completion flag drops the row. */
constexpr std::array<CollectibleQuest, 4> kCollectibleQuests{{
    {0x57C4540AU, 0U},
    {0x85CC476EU, 10762U},
    {0xB099029AU, 10766U},
    {0xC3535D63U, 10769U},
}};
/** One character has one customisation header, so both records carry the same 36 bytes. */
static_assert(character_record::layout::kHeaderBlockBytes.size()
              == layout::kCustomisationHeaderSize);
/** Character object B repeats the family-three periodic-reset block byte for byte. */
static_assert(sizeof(character_record::layout::PeriodicReset) == layout::kPeriodicResetRecordSize);

/** Validates the authored fields consumed by the character encoder. */
[[nodiscard]] bool valid(const state::CharacterState& state) noexcept {
    return state.soid != 0 && state.race <= state::CharacterRace::exo
           && state.gender <= state::CharacterGender::female
           && state.characterClass <= state::CharacterClass::warlock;
}

/** One new-item flag byte covers 8 consecutive inventory rows. */
constexpr std::size_t kBitsPerFlagByte = 8;
/** Character-object watermark for an occupied inventory row. */
constexpr std::int32_t kOccupiedRowWatermark = 1;

/**
 * Places every authored stack into the first free row of its character bucket.
 * @param object Receives the row, its new-item flag bit and its progress watermark.
 * @return False when a stack fails validation or its bucket has no free row.
 */
[[nodiscard]] bool place_character_stacks(const state::CharacterState& state,
                                          layout::Object& object) noexcept {
    if (!state::account::inventory::valid(state.stacks)) {
        return false;
    }
    for (std::size_t index = 0; index < state.stacks.count; ++index) {
        const auto& stack = state.stacks.values[index];
        state::build_data::items::Definition item{};
        state::build_data::items::details::Definition detail{};
        state::build_data::inventory::buckets::Descriptor bucket{};
        if (stack.quantity <= 0 || stack.mutationSerial < 0
            || static_cast<std::uint32_t>(stack.mutationSerial) >= state.nextInventorySerial
            || !state::build_data::find_item_definition_hash(stack.definitionHash, item)
            || !state::build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || detail.instancedDefinitionState
                   != state::build_data::items::details::InstancedDefinitionState::stackable
            || detail.equipmentSlot.has_value() || stack.quantity > detail.maxStackSize
            || !state::build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
            || bucket.arraySelector
                   != state::build_data::inventory::buckets::ArraySelector::character
            || bucket.slotCount == 0 || bucket.firstSlot >= object.inventoryItems.size()
            || bucket.slotCount > object.inventoryItems.size() - bucket.firstSlot) {
            return false;
        }
        const std::size_t end = static_cast<std::size_t>(bucket.firstSlot) + bucket.slotCount;
        std::size_t rowIndex = bucket.firstSlot;
        while (rowIndex < end
               && object.inventoryItems[rowIndex].definitionIndex != kEmptyDefinitionIndex) {
            ++rowIndex;
        }
        if (rowIndex == end) {
            return false;
        }
        auto& row = object.inventoryItems[rowIndex];
        row.definitionIndex = item.definitionIndex;
        row.quantity = stack.quantity;
        row.mutationSerial = stack.mutationSerial;
        object.newItemFlags[rowIndex / kBitsPerFlagByte] |= std::byte{1U}
                                                            << (rowIndex % kBitsPerFlagByte);
        object.instanceProgressWatermarks[rowIndex] = kOccupiedRowWatermark;
    }
    return true;
}

/**
 * Places collectible prerequisites in the character quest bucket. These stackable rows need no
 * item-instance resident.
 */
[[nodiscard]] bool place_collectible_quest_items(layout::Object& object) noexcept {
    std::optional<std::uint8_t> questBucketId;
    std::size_t nextRow = 0;
    std::size_t rowLimit = 0;
    for (const CollectibleQuest& quest : kCollectibleQuests) {
        if (quest.completionFlag != 0
            && (state::unlocks::records::claimed(quest.completionFlag)
                || state::unlocks::records::claimable(quest.completionFlag))) {
            continue;
        }
        state::build_data::items::Definition item{};
        state::build_data::items::details::Definition detail{};
        state::build_data::inventory::buckets::Descriptor bucket{};
        if (!state::build_data::find_item_definition_hash(quest.definitionHash, item)
            || !state::build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || detail.instancedDefinitionState
                   != state::build_data::items::details::InstancedDefinitionState::stackable
            || detail.maxStackSize < 1 || detail.equipmentSlot.has_value()
            || !state::build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
            || bucket.arraySelector
                   != state::build_data::inventory::buckets::ArraySelector::character
            || bucket.slotCount == 0 || bucket.firstSlot >= object.inventoryItems.size()
            || bucket.slotCount > object.inventoryItems.size() - bucket.firstSlot) {
            return false;
        }
        if (!questBucketId.has_value()) {
            questBucketId = item.bucketId;
            nextRow = bucket.firstSlot;
            rowLimit = bucket.firstSlot + bucket.slotCount;
        } else if (*questBucketId != item.bucketId || nextRow >= rowLimit) {
            return false;
        }
        while (nextRow < rowLimit
               && object.inventoryItems[nextRow].definitionIndex != kEmptyDefinitionIndex) {
            ++nextRow;
        }
        if (nextRow >= rowLimit) {
            return false;
        }

        inventory::layout::Entry& row = object.inventoryItems[nextRow];
        row.definitionIndex = item.definitionIndex;
        row.quantity = 1;
        object.newItemFlags[nextRow / kBitsPerFlagByte] |= std::byte{1U}
                                                           << (nextRow % kBitsPerFlagByte);
        object.instanceProgressWatermarks[nextRow] = kOccupiedRowWatermark;
        ++nextRow;
    }
    return true;
}

/** Validates the row-sorted loadout consumed by the character object. */
[[nodiscard]] bool valid(const loadout::ResolvedLoadout& resolvedLoadout) noexcept {
    if (resolvedLoadout.itemCount > resolvedLoadout.items.size()
        || resolvedLoadout.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }

    std::array<bool, layout::kEquipmentCapacity> occupiedEquipmentSlots{};
    std::array<std::uint64_t, loadout::kItemCapacity> instanceSoids{};
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        const loadout::ResolvedItem& item = resolvedLoadout.items[index];
        const instance::ResolvedInstance& itemInstance = item.instance;
        const bool validEquipmentSlot = item.equipmentSlot < occupiedEquipmentSlots.size();
        if (item.inventoryRow >= layout::kInventoryCapacity || item.quantity <= 0
            || itemInstance.instanceSoid == 0 || itemInstance.bounds.itemDefinitionCount == 0
            || itemInstance.bounds.itemDefinitionCount > instance::layout::kDefinitionIndexCapacity
            || itemInstance.baseDefinitionIndex == kEmptyDefinitionIndex
            || itemInstance.baseDefinitionIndex >= itemInstance.bounds.itemDefinitionCount
            || item.mutationSerial < 0
            || static_cast<std::uint32_t>(item.mutationSerial)
                   >= resolvedLoadout.nextInventorySerial
            || (item.equipped
                && (!validEquipmentSlot || occupiedEquipmentSlots[item.equipmentSlot]))
            || (index != 0 && resolvedLoadout.items[index - 1].inventoryRow >= item.inventoryRow)) {
            return false;
        }
        if (item.equipped) {
            occupiedEquipmentSlots[item.equipmentSlot] = true;
        }
        instanceSoids[index] = itemInstance.instanceSoid;
    }
    auto end = instanceSoids.begin() + static_cast<std::ptrdiff_t>(resolvedLoadout.itemCount);
    std::sort(instanceSoids.begin(), end);
    return std::adjacent_find(instanceSoids.begin(), end) == end;
}

/** Confirms the light summary describes exactly the resolved equipped instances. */
[[nodiscard]] bool
summary_matches_loadout(const loadout::ResolvedLoadout& resolvedLoadout,
                        const state::equipment::light::Evaluation& evaluation) noexcept {
    std::size_t summaryItemCount = 0;
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        summaryItemCount += static_cast<std::size_t>(resolvedLoadout.items[index].equipped);
    }
    std::size_t scoreCount = 0;
    for (const std::optional<state::equipment::light::ItemScore>& score : evaluation.character) {
        scoreCount += static_cast<std::size_t>(score.has_value());
    }
    if (summaryItemCount != scoreCount) {
        return false;
    }
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        const loadout::ResolvedItem& item = resolvedLoadout.items[index];
        if (!item.equipped) {
            continue;
        }
        const auto& score = evaluation.character[item.equipmentSlot];
        if (!score.has_value() || score->definitionIndex != item.instance.baseDefinitionIndex) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Encodes one selected-character object from live State and resolved installed mappings. */
bool encode(const state::CharacterState& state,
            const loadout::ResolvedLoadout& resolvedLoadout,
            const state::equipment::light::Evaluation& lightEvaluation,
            std::span<std::byte> output) noexcept {
    state::unlocks::Table unlocks;
    return state::unlocks::snapshot(unlocks)
           && encode(state, resolvedLoadout, lightEvaluation, output, unlocks);
}

/**
 * Character unlocks must match the live or prepared inventory view being encoded.
 * @param state Character identity and inventory to encode.
 * @param resolvedLoadout Item mappings for this character's inventory.
 * @param lightEvaluation Equipment light values for the same loadout.
 * @param output Receives the character object; unchanged on failure.
 * @param unlocks Unlock snapshot for this character, including any prepared quest value.
 * @return False when state, mappings, light values, or output bounds are invalid.
 */
bool encode(const state::CharacterState& state,
            const loadout::ResolvedLoadout& resolvedLoadout,
            const state::equipment::light::Evaluation& lightEvaluation,
            std::span<std::byte> output,
            const state::unlocks::Table& unlocks) noexcept {
    if (!valid(state) || !valid(resolvedLoadout)
        || !summary_matches_loadout(resolvedLoadout, lightEvaluation)
        || output.size() < layout::kObjectSize) {
        return false;
    }

    layout::Object object{};
    object.characterSoid = state.soid;
    object.identity.race = static_cast<std::int8_t>(state.race);
    object.identity.gender = static_cast<std::int8_t>(state.gender);
    object.identity.characterClass = static_cast<std::int8_t>(state.characterClass);
    std::memcpy(object.customisationHeader.data(),
                character_record::layout::kHeaderBlockBytes.data(),
                character_record::layout::kHeaderBlockBytes.size());
    object.lastOrbitedDestination = state.lastOrbitedDestination;
    // No previous activity is tracked, and a zero here would name the orbit activity.
    object.previousActivityIndex = kAbsentActivityIndex;
    // Absent, the client's transition classifier takes the orbit as its from side.
    object.activityOverrideIndex = kAbsentActivityIndex;
    object.currentActivityIndex = state.currentActivityIndex;
    object.previewMirrors.fill(state.previewAvailable ? kNativeTrue : kNativeFalse);
    object.contentBypass = state.contentBypass ? kNativeTrue : kNativeFalse;
    object.equippedTitleRecordIndex = state.equippedTitleRecordIndex;
    object.seenMessages.fill(kSeenMessageByte);
    // Both stamps are the last reset before sign-in. Zero would make the client run a daily and
    // a weekly rollover as soon as it accepts the object.
    character_record::layout::PeriodicReset reset{};
    reset.lastDailyResetSeconds = state.signInSeconds;
    reset.lastWeeklyResetSeconds = state.signInSeconds;
    std::memcpy(object.periodicResetRecord.data(), &reset, sizeof reset);
    for (inventory::layout::Entry& item : object.inventoryItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    // Sunrise retains no item stacks, so every physical row must read as unused.
    for (layout::ItemStackRow& stack : object.itemStacks) {
        stack.selector = kEmptyItemStackSelector;
    }
    // Use the supplied quest state even when the acquisition has not committed yet.
    for (std::size_t index = 0; index < object.acquiredFlags.size(); ++index) {
        object.acquiredFlags[index] = static_cast<std::byte>(
            index < unlocks.characterObjectFlags.size() ? unlocks.characterObjectFlags[index]
                                                        : std::uint8_t{});
    }
    for (std::size_t index = 0; index < object.objectiveValues.size(); ++index) {
        object.objectiveValues[index] =
            index < unlocks.characterObjectValues.size() ? unlocks.characterObjectValues[index] : 0;
    }
    if (!build_equipment_summary(lightEvaluation, object.equipmentSummary)) {
        return false;
    }
    if (!progression::key_bank(state::build_data::progressions::Scope::character,
                               object.progressions)) {
        return false;
    }
    // Rows are validated sorted, so the last row bounds the prefix the native walk must cover.
    object.inventoryRowCount =
        resolvedLoadout.itemCount == 0
            ? std::uint32_t{}
            : static_cast<std::uint32_t>(
                  resolvedLoadout.items[resolvedLoadout.itemCount - 1].inventoryRow + 1);
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        const loadout::ResolvedItem& item = resolvedLoadout.items[index];
        inventory::layout::Entry& inventoryRow = object.inventoryItems[item.inventoryRow];
        inventoryRow.definitionIndex = item.instance.baseDefinitionIndex;
        inventoryRow.instanceSoid = item.instance.instanceSoid;
        inventoryRow.quantity = item.quantity;
        inventoryRow.mutationSerial = item.mutationSerial;
        inventoryRow.flags = item.flags;
        // Badge state and instance watermarks are both addressed by inventory row.
        if (!item.seen) {
            object.newItemFlags[item.inventoryRow / kBitsPerFlagByte] |=
                std::byte{1U} << (item.inventoryRow % kBitsPerFlagByte);
        }
        object.instanceProgressWatermarks[item.inventoryRow] = kOccupiedRowWatermark;
        if (item.equipped) {
            object.equippedInstanceSoids[item.equipmentSlot] = item.instance.instanceSoid;
        }
    }
    if (!place_character_stacks(state, object) || !place_collectible_quest_items(object)) {
        return false;
    }

    // Commit only after validation so callers never receive a partially initialized object.
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::family4::character
