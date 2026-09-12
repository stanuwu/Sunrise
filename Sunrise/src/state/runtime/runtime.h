#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "../build_data/items/quest_initialization.h"
#include "../build_data/records/definition.h"
#include "state.h"

namespace sunrise::state::account::settings {

struct SettingsDelta;

} // namespace sunrise::state::account::settings

namespace sunrise::state {

/**
 * Assigns runtime SOIDs only to installed profile mod/shader rows which are socket action sources.
 * Currency, material, and consumable profile rows remain canonically non-instanced.
 */
[[nodiscard]] bool ensure_profile_item_identities() noexcept;

/** Why one attempt to canonicalize the "Emotes" collection item ended. */
enum class EmoteCollectionOutcome : std::uint8_t {
    /** Every character carries a sound collection item, either already or as of this call. */
    ready,
    /** The build data or account this reads is not published yet, so a retry is still owed. */
    notReady,
    /** The installed content does not carry the item this expects, so it can never be applied. */
    unsupported,
    /** The item could not be placed, so no character was changed and a retry is still owed. */
    failed,
};

/**
 * Grants each character the other 2 subclasses of its equipped subclass's class, placing missing
 * ones into unequipped inventory with native socket defaults. Idempotent: one already equipped or
 * already in inventory is left alone.
 * @return True when every such character holds its whole class, or there was nothing to check.
 */
[[nodiscard]] bool ensure_character_subclasses() noexcept;

/** Prepared subclass socket-entry selection for the equipped selected-character subclass. */
struct PendingSubclassSelection {
    /** Exact prepare-time character view used as the commit staleness guard. */
    CharacterState beforeCharacter{};
    /** Canonical after-image. Only one authored ability-entry field differs. */
    CharacterState afterCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t subclassInstanceSoid{};
    std::uint32_t subclassDefinitionHash{};
    std::size_t characterIndex{};
    std::uint16_t subclassDefinitionIndex{};
    std::uint16_t socketEntryListIndex{};
    /** Exact entry named by opcode 801. */
    std::uint8_t requestedEntry{};
    bool prepared{};
};

/**
 * Prepares one opcode-801 selection against the selected character's exact equipped subclass.
 * The installed socket-entry table maps the request to whichever of the character's 5 authored
 * picks competes in the same group; no class-specific node indices are authored in State.
 */
[[nodiscard]] bool prepare_subclass_selection(std::uint64_t subclassInstanceSoid,
                                              std::uint8_t requestedEntry,
                                              PendingSubclassSelection& mutation) noexcept;

/** Produces the complete uncommitted account after-image for a prepared subclass selection. */
[[nodiscard]] bool preview_subclass_selection(const PendingSubclassSelection& mutation,
                                              AccountState& after) noexcept;

/** Commits a prepared subclass selection behind the exact full-character staleness guard. */
[[nodiscard]] bool commit_subclass_selection(PendingSubclassSelection& mutation) noexcept;

/**
 * Equips the "Emotes" collection item in each character's emote slot, with seeded default lanes.
 * Idempotent and safe from more than one boundary: a sound copy is left alone and a broken one is
 * repaired in place, keeping its instance identity.
 */
[[nodiscard]] EmoteCollectionOutcome ensure_character_emote_collection() noexcept;

/** Direction of one checked character equipment mutation. */
enum class EquipmentMutationKind : std::uint8_t {
    none,
    equip,
    unequip,
};

/** Prepared character-inventory mutation kept private until its response and update both fit. */
struct PendingEquipmentSwap {
    /** Exact prepare-time character view used as the commit staleness guard. */
    CharacterState beforeCharacter{};
    /** Canonical after-image, including every row-change mutation generation. */
    CharacterState afterCharacter{};
    std::uint64_t characterSoid{};
    std::uint64_t requestedInstanceSoid{};
    std::uint64_t previousInstanceSoid{};
    std::size_t characterIndex{};
    std::size_t equipmentSlotIndex{};
    std::size_t inventoryIndex{};
    std::size_t movedItemCount{};
    std::uint8_t nativeEquipmentSlot{};
    EquipmentMutationKind kind{};
    bool prepared{};
};

/** Prepared selected-character inventory insertion kept private until its reply and push fit. */
struct PendingItemAcquisition {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    /** Profile material view, before and after charging the native requirement set. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeProfileItems{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        afterProfileItems{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t acquiredInstanceSoid{};
    std::uint32_t acquiredDefinitionHash{};
    std::uint32_t materialRequirementSetHash{};
    std::size_t characterIndex{};
    std::size_t expectedInventoryCount{};
    std::size_t expectedProfileItemCount{};
    std::size_t afterProfileItemCount{};
    std::size_t inventoryIndex{};
    std::uint16_t collectibleIndex{};
    std::uint16_t inventoryRow{};
    std::uint8_t equipmentSlot{};
    std::uint8_t materialRequirementCount{};
    bool profileChanged{};
    /** Skips Collections revalidation for direct rewards. */
    bool directGrant{};
    build_data::items::QuestInitialization questInitialization{};
    std::int32_t previousQuestValue{};
    bool prepared{};

    /**
     * Account-scoped quest writes need an account update even when no materials were charged.
     * @return True for a profile inventory change or an unset account-scoped quest value.
     */
    [[nodiscard]] bool updates_account() const noexcept {
        return profileChanged
               || (questInitialization.scope
                       == build_data::items::QuestInitialization::Scope::account
                   && previousQuestValue == build_data::items::kUnsetQuestValue);
    }
};

/** One profile row an exchange changed, named the way the account's change ring names it. */
struct ProfileStackChange {
    std::int32_t mutationSerial{};
    std::int32_t afterQuantity{};
};

/** Rows one exchange may announce. Shader recycling announces two: Glimmer and Legendary Shards. */
inline constexpr std::size_t kProfileStackChangeCapacity = 4;

/** Prepared account-profile stack insertion kept private until its reply and account upsert fit. */
struct PendingProfileItemAcquisition {
    /** Exact profile inventory observed while preparing the mutation. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeItems{};
    /** Canonical profile inventory after incrementing or appending one stack. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        afterItems{};
    std::uint64_t accountSoid{};
    /** Stable profile-row source identity, preserved for increments and allocated for appends. */
    std::uint64_t acquiredInstanceSoid{};
    std::uint32_t acquiredDefinitionHash{};
    std::uint32_t materialRequirementSetHash{};
    std::size_t expectedItemCount{};
    std::size_t afterItemCount{};
    std::size_t profileIndex{};
    std::int32_t previousQuantity{};
    std::int32_t acquiredQuantity{};
    std::int32_t previousMutationSerial{};
    std::int32_t acquiredMutationSerial{};
    std::uint16_t collectibleIndex{};
    std::uint8_t bucketId{};
    std::uint8_t materialRequirementCount{};
    /**
     * Rows this mutation announces to the account's change ring, which is the only way the Client
     * is told of a currency gain. Empty marks an ordinary acquisition, which announces its one
     * acquired row instead; non-empty marks an exchange.
     */
    std::array<ProfileStackChange, kProfileStackChangeCapacity> changes{};
    std::size_t changeCount{};
    /** True only for installed profile mod/shader rows materialized as Family-4 residents. */
    bool actionSource{};
    bool appended{};
    /** Skips Collections revalidation for direct rewards. */
    bool directGrant{};
    bool prepared{};
};

/** Prepared fixed package expansion kept private until every object and response byte fits. */
struct PendingDirectItemBundle {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t firstInstanceSoid{};
    std::uint32_t sourceDefinitionHash{};
    std::size_t characterIndex{};
    std::size_t expectedInventoryCount{};
    std::size_t itemCount{};
    bool prepared{};
};

/** Shared batch capacity covers both Triumph rewards and the nine-row Season package. */
inline constexpr std::size_t kRecordRewardGrantCapacity = 9;
static_assert(kRecordRewardGrantCapacity >= build_data::records::kRewardPerRecordCapacity);

/** One direct item requested by a record reward policy. */
struct DirectRecordReward {
    std::uint16_t itemDefinitionIndex{};
    std::int32_t quantity{};
};

enum class RecordRewardKind : std::uint8_t {
    characterInstance,
    characterStack,
    profileStack,
};

/** Native row identity of one item inside a prepared record-reward batch. */
struct PreparedRecordReward {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::size_t stateIndex{};
    std::int32_t quantity{};
    std::int32_t afterQuantity{};
    std::int32_t mutationSerial{};
    std::uint16_t inventoryRow{};
    RecordRewardKind kind{};
    bool appendedProfileResident{};
};

/** A reward grant that claims no record carries this instead of a record row. */
inline constexpr std::uint16_t kUnclaimedRecordIndex = 0xFFFFU;

/** Record claim and all of its item rows committed as one transaction. */
struct PendingRecordRewardGrant {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeProfileItems{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        afterProfileItems{};
    std::array<PreparedRecordReward, kRecordRewardGrantCapacity> rewards{};
    /** Record claimed with this grant, already written to the banks, or the unclaimed row. */
    std::uint16_t claimedRecordIndex{kUnclaimedRecordIndex};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t characterIndex{};
    std::size_t beforeProfileItemCount{};
    std::size_t afterProfileItemCount{};
    std::size_t rewardCount{};
    bool prepared{};
};

/** One uncommitted Season reward and the exact native row or bundle it will claim. */
struct PendingSeasonPassReward {
    std::variant<PendingItemAcquisition,
                 PendingProfileItemAcquisition,
                 PendingDirectItemBundle,
                 PendingRecordRewardGrant>
        grant{};
    std::uint32_t sourceDefinitionHash{};
    std::uint16_t rewardIndex{};
    bool prepared{};
};

/** One profile material actually credited by a prepared dismantle. */
struct DismantleReward {
    std::uint32_t definitionHash{};
    std::size_t profileIndex{};
    std::int32_t quantity{};
    std::int32_t afterQuantity{};
    std::int32_t mutationSerial{};
};

/** Dismantle feedback can publish every bounded server-authored policy row. */
inline constexpr std::size_t kDismantleRewardCapacity = kDismantleRewardPolicyCapacity;

/** Prepared selected-character inventory removal kept private until its reply and push fit. */
struct PendingItemDismantle {
    /** Exact prepare-time character view used as the commit staleness guard. */
    CharacterState beforeCharacter{};
    /** Canonical dense inventory after-image, including row-change mutation generations. */
    CharacterState afterCharacter{};
    /** Exact profile material view observed before and after applying the dismantle payout. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeProfileItems{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        afterProfileItems{};
    std::array<DismantleReward, kDismantleRewardCapacity> rewards{};
    account::inventory::Item dismantledItem{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t dismantledInstanceSoid{};
    std::size_t characterIndex{};
    std::size_t expectedInventoryCount{};
    std::size_t expectedProfileItemCount{};
    std::size_t afterProfileItemCount{};
    std::size_t inventoryIndex{};
    std::size_t movedInventoryItemCount{};
    std::size_t rewardCount{};
    std::uint16_t inventoryRow{};
    std::uint8_t equipmentSlot{};
    bool profileChanged{};
    bool prepared{};
};

/** Prepared ordinary-socket selection for one selected-character item instance. */
struct PendingSocketPlug {
    /** Exact prepare-time character view used as the commit staleness guard. */
    CharacterState beforeCharacter{};
    /** Canonical after-image. Only the target item's authored socket block differs. */
    CharacterState afterCharacter{};
    /** Exact account-wide material balances observed before applying the installed cost set. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeProfileItems{};
    /** Canonical material balances after every consuming row in the installed cost set. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        afterProfileItems{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t targetInstanceSoid{};
    std::uint32_t targetDefinitionHash{};
    std::uint32_t plugDefinitionHash{};
    std::uint32_t materialRequirementSetHash{};
    std::size_t characterIndex{};
    std::size_t expectedProfileItemCount{};
    std::size_t afterProfileItemCount{};
    /** Equipment semantic index or dense inventory index, selected by `targetEquipped`. */
    std::size_t itemIndex{};
    std::uint16_t targetDefinitionIndex{};
    /** Plug that lands in the lane. Differs from the request only for a rolled socket. */
    std::uint16_t plugDefinitionIndex{};
    /** Plug the Client asked for, which decides the pool check and the material charge. */
    std::uint16_t requestedPlugDefinitionIndex{};
    std::uint16_t materialRequirementSetIndex{0xFFFFU};
    std::uint8_t socketLane{};
    std::uint8_t targetBucketId{};
    std::uint8_t plugBucketId{};
    std::uint8_t materialRequirementCount{};
    bool profileChanged{};
    bool targetEquipped{};
    bool prepared{};
};

/** Prepared accumulated item-state change for one selected-character item instance. */
struct PendingItemState {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t characterSoid{};
    std::uint64_t targetInstanceSoid{};
    std::size_t characterIndex{};
    /** Equipment semantic index or dense inventory index, selected by `targetEquipped`. */
    std::size_t itemIndex{};
    std::uint16_t targetDefinitionIndex{};
    std::uint32_t beforeFlags{};
    std::uint32_t afterFlags{};
    bool targetEquipped{};
    bool prepared{};
};

/** Prepared artifact ownership transition for the selected character. */
struct PendingArtifactPurchase {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t characterIndex{};
    std::uint32_t beforeMask{};
    std::uint32_t afterMask{};
    std::uint16_t saleIndex{};
    bool prepared{};
};

/** Item residents whose authored artifact sockets were cleared by one reset. */
struct ArtifactResetResult {
    std::array<std::uint64_t,
               account::inventory::kEquipmentSlotCount + account::inventory::kCharacterItemCapacity>
        instanceSoids{};
    std::size_t instanceCount{};
};

/** Prepared current-activity change for the selected character, private until it publishes. */
struct PendingCurrentActivity {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t characterSoid{};
    std::size_t characterIndex{};
    std::uint16_t activityIndex{};
    bool prepared{};
};

/** Result of validating one sparse settings writeback against authoritative State. */
enum class SettingsUpdateDisposition : std::uint8_t {
    rejected,
    acceptedNoChange,
    preparedMutation,
};

/** Complete checked settings before/after images held until the BAP transaction commits. */
struct PendingSettingsUpdate {
    account::settings::AccountSettings beforeSettings{};
    account::settings::AccountSettings afterSettings{};
    std::uint64_t accountSoid{};
    bool prepared{};
};

/**
 * Loads cached build data and generates secrets with Sunrise's authored activity defaults.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @return True when the cached data passes its checks and every secret is generated.
 */
[[nodiscard]] bool initialize(void* module = nullptr) noexcept;

/**
 * Loads cached build data and publishes fixed activity defaults in one step.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param activityDefaults Complete local fallback policy from immutable Core settings.
 * @return True when account, defaults, cached data, and generated secrets are valid.
 */
[[nodiscard]] bool
initialize(void* module, const activity::defaults::ActivityDefaults& activityDefaults) noexcept;

/** Securely clears State, including activity destinations and matchmaking descriptors. */
void shutdown() noexcept;

/** @return Immutable generated SignOn session fields. */
[[nodiscard]] const SignOnState& sign_on() noexcept;

[[nodiscard]] bool publish_bootstrap_token(std::span<const std::byte> token) noexcept;

/**
 * Records when the account signed in.
 * Every character record publishes this as its last applied daily and weekly reset.
 * @param seconds Unix seconds taken when the SignOn success is answered.
 */
void publish_sign_in_time(std::uint64_t seconds) noexcept;

/** @return Immutable generated BAP session fields. */
[[nodiscard]] const BapState& bap() noexcept;

/**
 * Generates one connection's own secure-channel material.
 * Two links sharing a key and a starting nonce would encrypt different plaintexts under the same
 * pair, so every accepted connection gets its own.
 * @param output Cleared, then filled with a fresh nonce, session key and envelope IV.
 * @return True when the system generated every byte.
 */
[[nodiscard]] bool new_bap_session(BapState& output) noexcept;

/**
 * Stores the active nonzero account key when the account remains complete.
 * @param primarySoid Account key selected by the local Client.
 * @return False when the key or resulting account State is invalid.
 */
[[nodiscard]] bool set_primary_soid(std::uint64_t primarySoid) noexcept;

/**
 * Permanently closes the process-local one-time profile-setup gate for the active account.
 *
 * The transition is monotonic: repeated profile-setting writes after completion are harmless.
 * @return False only when no complete active account can be updated.
 */
[[nodiscard]] bool complete_profile_setup() noexcept;

/**
 * Moves the selection to one authored character.
 * The Client names its pick only in the select-character request, so this is where a player's
 * choice enters State.
 * @param characterSoid Picked character key, which must name an authored character.
 * @param changed Receives whether the selection moved to a different character.
 * @return False when no authored character carries that key.
 */
[[nodiscard]] bool set_selected_character(std::uint64_t characterSoid, bool& changed) noexcept;

/**
 * Stores the selected character's equipped title row. The caller proves the record is a claimed
 * title; this only writes it.
 * @param recordIndex Title record row, or kUnequippedTitleRecordIndex to clear it.
 * @param characterSoid Receives the selected character's key, or zero on failure.
 * @param changed Receives whether the stored row moved.
 * @return False when no character is selected or the account would stop being valid.
 */
[[nodiscard]] bool
set_selected_title(std::uint16_t recordIndex, std::uint64_t& characterSoid, bool& changed) noexcept;

/**
 * Prepares an equip operation for one unequipped instance on the selected character.
 * An occupied slot is swapped; an empty semantic slot receives the requested item directly.
 * @param requestedInstanceSoid Unequipped item instance selected by the Client.
 * @param mutation Gets the checked after-image without changing account State.
 * @return True when the instance is owned, unequipped, and maps to one native equipment slot.
 */
[[nodiscard]] bool prepare_equipment_swap(std::uint64_t requestedInstanceSoid,
                                          PendingEquipmentSwap& mutation) noexcept;

/**
 * Prepares an unequip operation for one equipped selected-character instance.
 * The item is inserted before existing inventory items in its native bucket so their published
 * rows remain stable. Native slots without a proven semantic State mapping are rejected.
 *
 * @param requestedInstanceSoid Equipped item instance selected by the Client.
 * @param mutation Gets the checked after-image without changing account State.
 * @return True when the instance is equipped and the dense character inventory has room.
 */
[[nodiscard]] bool prepare_equipment_unequip(std::uint64_t requestedInstanceSoid,
                                             PendingEquipmentSwap& mutation) noexcept;

/**
 * Commits a prepared equipment mutation only while the full captured character still matches.
 *
 * @param mutation Prepared mutation, always cleared before this function returns.
 * @return True when the equip or unequip commits atomically and leaves the account valid.
 */
[[nodiscard]] bool commit_equipment_swap(PendingEquipmentSwap& mutation) noexcept;

/**
 * Prepares one installed equippable definition as a new selected-character inventory instance.
 *
 * Native-default sockets, a unique runtime SOID, and the selected character's current item level
 * are used. Full loadout resolution is the authoritative bucket-capacity check.
 *
 * @param collectibleIndex Collections row the Client pulled from.
 * @param definitionHash Installed item definition requested by the Client.
 * @param mutation Gets a checked after-image without changing account State.
 * @return True when the item and every existing loadout row resolve with one free native row.
 */
[[nodiscard]] bool prepare_item_acquisition(std::uint16_t collectibleIndex,
                                            std::uint32_t definitionHash,
                                            PendingItemAcquisition& mutation) noexcept;

/** Prepares a direct character-item grant without a Collections charge. */
[[nodiscard]] bool prepare_item_acquisition_for_item(std::uint16_t itemDefinitionIndex,
                                                     PendingItemAcquisition& mutation) noexcept;

/** Prepares one fixed wrapper expansion without changing account State. */
[[nodiscard]] bool prepare_direct_item_bundle(std::uint32_t sourceDefinitionHash,
                                              std::span<const std::uint16_t> itemDefinitionIndices,
                                              PendingDirectItemBundle& mutation) noexcept;

/** Builds the full account after-image while a prepared bundle remains current. */
[[nodiscard]] bool preview_direct_item_bundle(const PendingDirectItemBundle& mutation,
                                              AccountState& after) noexcept;

/** Atomically commits one prepared reward grant and its durable Season claim. */
[[nodiscard]] bool commit_season_pass_reward(PendingSeasonPassReward& mutation) noexcept;

/** Atomically commits one prepared Triumph reward and its durable record claim. */
[[nodiscard]] bool commit_record_reward(PendingRecordRewardGrant& mutation) noexcept;

/**
 * Prepares all direct reward rows over one shared account after-image.
 * @param rewards Item rows the record grants.
 * @param claimedRecordIndex Record already claimed in the banks, or kUnclaimedRecordIndex.
 * @param mutation Receives the prepared grant.
 * @return True when every row fits the account after-image.
 */
[[nodiscard]] bool prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                                               std::uint16_t claimedRecordIndex,
                                               PendingRecordRewardGrant& mutation) noexcept;

/** Builds the full account after-image while a record reward remains current. */
[[nodiscard]] bool preview_record_reward_grant(const PendingRecordRewardGrant& mutation,
                                               AccountState& after) noexcept;

/** Reserves the selected character's next mutation serial for a transient inventory update. */
[[nodiscard]] bool
reserve_selected_character_inventory_serial(std::int32_t& mutationSerial) noexcept;

/**
 * Preview inventory and quest values together without changing the save.
 * @param mutation Prepared acquisition checked against current saved state.
 * @param after Receives the candidate account; use only on success.
 * @param afterUnlocks Receives matching account and selected-character unlocks on success.
 * @return False when the acquisition is stale or its saved unlocks cannot be read.
 */
[[nodiscard]] bool preview_item_acquisition(const PendingItemAcquisition& mutation,
                                            AccountState& after,
                                            unlocks::Table& afterUnlocks) noexcept;

/**
 * Inventory and first-step state share one transaction; failure rolls both back.
 * @param mutation Prepared grant consumed on either success or failure.
 * @return True when both writes commit against the unchanged prepared state.
 */
[[nodiscard]] bool commit_item_acquisition(PendingItemAcquisition& mutation) noexcept;

/**
 * Prepares one installed profile-owned stackable definition for a Collections pull.
 *
 * An existing non-full stack is incremented. Otherwise a new dense State entry is appended only
 * when the installed profile bucket still owns a free native row.
 *
 * @param collectibleIndex Collections row the Client pulled from.
 * @param definitionHash Installed stackable definition requested by the Client.
 * @param mutation Gets the checked profile before/after images without changing account State.
 * @return True when the definition belongs to the main profile array and one unit fits.
 */
[[nodiscard]] bool
prepare_profile_item_acquisition(std::uint16_t collectibleIndex,
                                 std::uint32_t definitionHash,
                                 PendingProfileItemAcquisition& mutation) noexcept;

/** Prepares a direct profile-stack grant without a Collections charge. */
[[nodiscard]] bool
prepare_profile_item_acquisition_for_item(std::uint16_t itemDefinitionIndex,
                                          std::int32_t quantity,
                                          PendingProfileItemAcquisition& mutation) noexcept;

/**
 * Materializes a prepared profile acquisition over the current account only while its complete
 * profile-inventory view is unchanged. This is the account object encoded before commit.
 *
 * @param mutation Prepared mutation that remains owned by the transaction.
 * @param after Gets the exact full-account after-image used by the Family-4 upsert.
 * @return True when the mutation is whole and its prepare-time profile remains current.
 */
[[nodiscard]] bool preview_profile_item_acquisition(const PendingProfileItemAcquisition& mutation,
                                                    AccountState& after) noexcept;

/**
 * Commits a prepared profile stack insertion only while its prepare-time profile remains current.
 *
 * @param mutation Prepared mutation, always cleared before this function returns.
 * @return True when the stack update commits atomically and leaves the whole account valid.
 */
[[nodiscard]] bool
commit_profile_item_acquisition(PendingProfileItemAcquisition& mutation) noexcept;

/**
 * Prepares removal of one unequipped instance from the selected character.
 * The authored inventory prefix is compacted. Any surviving item whose installed native row
 * changes receives a fresh mutation generation. Equipped items are never accepted.
 * @param instanceSoid Unequipped item-instance key selected by the Client.
 * @param mutation Gets checked before/after images without changing account State.
 * @return True when the selected character uniquely owns it and both loadouts resolve.
 */
[[nodiscard]] bool prepare_item_dismantle(std::uint64_t instanceSoid,
                                          PendingItemDismantle& mutation) noexcept;

/** Builds the exact account after-image while a prepared dismantle remains current. */
[[nodiscard]] bool preview_item_dismantle(const PendingItemDismantle& mutation,
                                          AccountState& after) noexcept;

/**
 * Commits a prepared inventory removal only while the complete prepare-time character view is
 * unchanged.
 *
 * @param mutation Prepared mutation, always cleared before this function returns.
 * @return True when the removal commits atomically and leaves the whole account valid.
 */
[[nodiscard]] bool commit_item_dismantle(PendingItemDismantle& mutation) noexcept;

/**
 * Prepares one exact opcode-903 ordinary-socket selection on a selected-character item.
 * The target may be equipped or unequipped. Native defaults are materialized into a complete
 * authored socket block, then only the requested lane changes; everything else stays byte-stable.
 * @param targetInstanceSoid Selected-character item-instance key named by the Client.
 * @param socketLane Zero-based ordinary socket lane.
 * @param plugDefinitionIndex Installed plug-definition row selected by the Client.
 * @param mutation Gets the checked before/after images without changing account State.
 * @return True when ownership, item detail, lane, plug compatibility, and both loadouts validate.
 */
[[nodiscard]] bool prepare_socket_plug(std::uint64_t targetInstanceSoid,
                                       std::uint8_t socketLane,
                                       std::uint16_t plugDefinitionIndex,
                                       PendingSocketPlug& mutation) noexcept;

/**
 * Prepares one ordinary-socket selection for an exact character-screen item selector.
 * The resolved instance runs through the same checked transition as an instance-addressed action,
 * so acquired and unequipped items do not depend on a coincidental menu-row ordinal.
 * @param instanceIdentityToken Item-instance identity decoded from the opcode-1901 selector.
 * @param requestedSocketLane Native socket action lane; compatibility resolves the physical lane.
 * @param plugDefinitionIndex Installed plug-definition row selected by the Client.
 * @param mutation Gets the checked before/after images without changing account State.
 * @return True when one item matches, the plug resolves to that lane, and the transition is valid.
 */
[[nodiscard]] bool prepare_character_selector_socket_plug(std::uint64_t instanceIdentityToken,
                                                          std::uint8_t requestedSocketLane,
                                                          std::uint16_t plugDefinitionIndex,
                                                          PendingSocketPlug& mutation) noexcept;

/** Produces the complete uncommitted account after-image for a prepared socket transaction. */
[[nodiscard]] bool preview_socket_plug(const PendingSocketPlug& mutation,
                                       AccountState& after) noexcept;

/**
 * Commits a prepared socket selection only while the complete prepare-time character is unchanged.
 * @param mutation Prepared mutation, always cleared before this function returns.
 * @return True when the exact canonical transition commits atomically.
 */
[[nodiscard]] bool commit_socket_plug(PendingSocketPlug& mutation) noexcept;

/** Prepares one complete native item-state value for an owned selected-character instance. */
[[nodiscard]] bool prepare_item_state(std::uint64_t targetInstanceSoid,
                                      std::uint16_t targetDefinitionIndex,
                                      std::uint32_t flags,
                                      PendingItemState& mutation) noexcept;

/** Commits one prepared item-state change behind an exact full-character staleness guard. */
[[nodiscard]] bool commit_item_state(PendingItemState& mutation) noexcept;

/**
 * Prepares the selected character's current activity, family-4 `+45896`, without changing State.
 * @param activityIndex Activity the character is launching into.
 * @param mutation Gets the checked after-image.
 * @return True when a character is selected and the value changes.
 */
[[nodiscard]] bool prepare_current_activity(std::uint16_t activityIndex,
                                            PendingCurrentActivity& mutation) noexcept;

/** Commits one prepared current-activity change behind an exact character staleness guard. */
[[nodiscard]] bool commit_current_activity(PendingCurrentActivity& mutation) noexcept;

/**
 * Merges and validates a sparse WS-701 settings update without publishing it.
 * @param delta Supported fields decoded from one reflected settings request.
 * @param mutation Receives a complete before/after pair only when State would change.
 * @return Rejection, an accepted no-op, or a prepared mutation.
 */
[[nodiscard]] SettingsUpdateDisposition
prepare_settings_update(const account::settings::SettingsDelta& delta,
                        PendingSettingsUpdate& mutation) noexcept;

/**
 * Publishes one prepared settings after-image behind account-key and settings staleness guards.
 * @param mutation Prepared update, always cleared before this function returns.
 * @return True when the after-image was already current or was committed successfully.
 */
[[nodiscard]] bool commit_settings_update(PendingSettingsUpdate& mutation) noexcept;

/** One credited side of a vendor exchange: an authored profile stack and how much to add. */
struct ProfileExchangePayout {
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
};

/**
 * Prepares one vendor recycle row: charges the stack it names and credits what it pays out.
 * It rides the profile-stack mutation because the change ring is the only way the Client is told
 * of a gain. Each credited row gets a fresh serial; only an already-held payout stack is credited.
 * @param costDefinitionHash Stack the row charges against.
 * @param costQuantity Units of it the row consumes.
 * @param payouts Stacks to credit, each clamped to its own native stack limit.
 * @param mutation Gets the checked profile before/after images without changing account State.
 * @return True only when the charge and every credit fit and the whole account stayed valid.
 */
[[nodiscard]] bool prepare_vendor_exchange(std::uint32_t costDefinitionHash,
                                           std::int32_t costQuantity,
                                           std::span<const ProfileExchangePayout> payouts,
                                           PendingProfileItemAcquisition& mutation) noexcept;

/** @return A copy of the active account state, read under the lock. */
[[nodiscard]] AccountState account_snapshot() noexcept;

/**
 * Copies the evaluated content state and adds build-derived catalyst completion overrides.
 * @param output Receives one complete Family-5 snapshot on success.
 * @return False when the fixed override banks cannot hold the complete state.
 */
[[nodiscard]] bool investment_snapshot(InvestmentState& output) noexcept;

/** Seasonal artifact item definition, whose equipped row carries the Power bonus stat. */
inline constexpr std::uint32_t kSeasonalArtifactItemHash = 0x613A3DA6U;

/** Native progression row carrying the seasonal artifact Power ladder. */
inline constexpr std::uint16_t kArtifactPowerProgressionIndex = 38;
/** Native progression row carrying the seasonal artifact unlock-point ladder. */
inline constexpr std::uint16_t kArtifactUnlockProgressionIndex = 39;

/** @return Seasonal XP published in the account progression bank. */
[[nodiscard]] std::int32_t seasonal_experience() noexcept;

/** Publishes every seasonal value the seeded XP and artifact ownership imply. */
[[nodiscard]] bool seed_seasonal_progression() noexcept;

/** @return One-based Season of Arrivals rank the published XP earns. */
[[nodiscard]] std::uint16_t seasonal_rank() noexcept;

/** @return Account-wide Power bonus published by the seasonal artifact. */
[[nodiscard]] std::uint16_t artifact_power_bonus() noexcept;

/**
 * Adds base XP to the seasonal lanes and republishes every value derived from the total.
 * @param amount Positive XP to grant.
 * @return False when the amount is not positive or the total would overflow.
 */
[[nodiscard]] bool grant_seasonal_experience(std::int32_t amount) noexcept;

/** @param rewardIndex Native reward-array index. @return True when the row is claimed. */
[[nodiscard]] bool season_pass_reward_claimed(std::uint16_t rewardIndex) noexcept;

/**
 * Claims one Season pass reward row into the account flag its row names.
 * @param rewardIndex Native reward-array index.
 * @return False when the row names no flag or is already claimed.
 */
[[nodiscard]] bool claim_season_pass_reward(std::uint16_t rewardIndex) noexcept;

/** Undoes one Season pass claim so a refused commit cannot leave it held. */
void revoke_season_pass_reward(std::uint16_t rewardIndex) noexcept;

/** @return Purchased artifact sale rows, one bit per row. */
[[nodiscard]] std::uint32_t artifact_mod_mask() noexcept;

/** Replaces the exact published artifact mask, refusing when it already moved. */
[[nodiscard]] bool replace_artifact_mod_mask(std::uint32_t expected,
                                             std::uint32_t replacement) noexcept;

/** Writes one affordable artifact purchase and keeps its before-image for the commit. */
[[nodiscard]] bool prepare_artifact_mod_unlock(std::uint16_t saleIndex,
                                               PendingArtifactPurchase& mutation) noexcept;

/** Keeps a prepared artifact purchase only while its character and mask are still current. */
[[nodiscard]] bool commit_artifact_mod_unlock(PendingArtifactPurchase& mutation) noexcept;

/** Charges Glimmer, removes artifact mods, and refunds every spent unlock point. */
[[nodiscard]] bool reset_artifact(std::int32_t glimmerCost, ArtifactResetResult& result) noexcept;

} // namespace sunrise::state
