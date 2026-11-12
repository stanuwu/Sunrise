#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "../account/account_context.h"
#include "../build_data/items/quest_initialization.h"
#include "../build_data/records/definition.h"
#include "../build_data/rewards/definition.h"
#include "../build_data/vendors/definition.h"
#include "../unlocks/definition.h"
#include "state.h"
#include "state_vendor_reputation_runtime.h"

namespace sunrise::state::unlocks {
struct Table;
}

namespace sunrise::state::account::settings {

struct SettingsDelta;

} // namespace sunrise::state::account::settings

namespace sunrise::state {

/**
 * @return True when the reward grant path can place one installed item: wrappers, account perks,
 * stackable profile items and any item in a character bucket.
 */
[[nodiscard]] bool item_grantable(std::uint16_t itemIndex) noexcept;
/**
 * How much of a larger grant one placement carries: one instance, draw or perk, or up to one full
 * stack. A large quantity therefore arrives as a run of ordinary grants.
 */
[[nodiscard]] std::int32_t item_grant_step(std::uint16_t itemIndex,
                                           std::int32_t remaining) noexcept;
/**
 * The most of one item a grant can leave held at once: one step in every row of the item's own
 * bucket and of the recovery bucket that takes its overflow. Past that, a step only evicts or
 * drops an earlier one. An account perk holds one. A wrapper's draws land wherever its pool sends
 * them, so a wrapper is bounded only by the largest quantity a grant carries.
 * @return Zero when the item has no installed bucket.
 */
[[nodiscard]] std::int32_t item_grant_capacity(std::uint16_t itemIndex) noexcept;
/**
 * Assigns runtime SOIDs only to installed profile mod/shader rows which are socket action sources.
 * Currency, material, and consumable profile rows remain canonically non-instanced.
 */
[[nodiscard]] bool ensure_profile_item_identities() noexcept;

/** Why one pass over uninstalled content ended. */
enum class ParkingOutcome : std::uint8_t {
    /** Every account row resolves against the installed content, as of this call. */
    ready,
    /** The build data or account this reads is not published yet, so a retry is still owed. */
    notReady,
    /** A database step failed; the account is unchanged. */
    failed,
};

/**
 * Moves account rows whose definition, or a socket plug's, is not installed into parked_items,
 * and moves parked rows whose content is installed again back. Nothing is deleted, so removing a
 * mod and adding it back keeps its items.
 */
[[nodiscard]] ParkingOutcome reconcile_installed_content() noexcept;

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

    bool operator==(const ProfileStackChange&) const = default;
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
    bool prepared{};
};

/** Shared batch capacity bounds one reward transaction. */
inline constexpr std::size_t kRecordRewardGrantCapacity = build_data::rewards::kGrantCapacity;
static_assert(kRecordRewardGrantCapacity >= build_data::records::kRewardPerRecordCapacity);

/** One direct item requested by a record reward policy. */
struct DirectRecordReward {
    std::uint16_t itemDefinitionIndex{};
    std::int32_t quantity{};
    std::span<const build_data::rewards::SocketOverride> sockets{};
    bool acquireUnlock{};
};

enum class RecordRewardKind : std::uint8_t {
    characterInstance,
    characterStack,
    profileStack,
    accountUnlock,
};

/** The account unlock bank a transaction row belongs to. */
enum class AccountUnlockBank : std::uint8_t { values, flags };

/** One account unlock row a transaction writes, and the value its preparation observed. */
struct AccountUnlockWrite {
    AccountUnlockBank bank{AccountUnlockBank::values};
    std::uint16_t index{};
    std::int32_t previousValue{};
    std::int32_t value{};

    bool operator==(const AccountUnlockWrite&) const = default;
};

/** One requested item inside a prepared record-reward batch; its rows are the batch's rows. */
struct PreparedRecordReward {
    /** The new instance's identity, or zero for a stack or an instance that found no row. */
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    /** Quantity that found a row; the client drops the rest. */
    std::int32_t placedQuantity{};
    RecordRewardKind kind{};
    std::uint16_t acquiredFlag{build_data::rewards::kAbsent};
    std::uint8_t previousFlag{};
    /** First quest step a placed character instance starts, and the saved value it observed. */
    build_data::items::QuestInitialization quest{};
    std::int32_t previousQuestValue{};
    /**
     * The oven balance a placed ingredient pickup credits. Its observed value follows an earlier
     * pickup of the same balance in the batch.
     */
    std::optional<AccountUnlockWrite> ingredientCredit{};
    /** The tutorial quest value a placed oven writes to open its ingredient chooser, if any. */
    std::optional<AccountUnlockWrite> chooserReadiness{};
};

/** The saved array a granted or edited row belongs to. */
enum class GrantedRowArray : std::uint8_t { profile, characterItem, characterStack };

/**
 * One row a grant created or grew, or a row its FIFO eviction moved. Each such change takes a new
 * mutation serial, so the serial names the row in the grant's after-images.
 */
struct GrantedRow {
    GrantedRowArray array{};
    std::int32_t mutationSerial{};
    /** The change created the row rather than growing a held stack. */
    bool newRow{};
};

/** One consumed wrapper's acquisition flag and the value its preparation observed. */
struct PreparedRewardFlag {
    std::uint16_t index{};
    std::uint8_t previousValue{};
};

/** Flagged wrappers one grant consumes. Policy: the grant row bound; a draw over it is refused. */
inline constexpr std::size_t kRewardWrapperFlagCapacity = build_data::rewards::kGrantCapacity;

/** A reward grant that claims no record carries this instead of a record row. */
inline constexpr std::uint16_t kUnclaimedRecordIndex = 0xFFFFU;

/**
 * A saved value a redemption moves on, such as its quest set's step, and the value its
 * preparation observed.
 */
struct PreparedQuestStep {
    /** The value it moves to and its saved row; scope none when nothing moves. */
    build_data::items::QuestInitialization next{};
    std::int32_t previousValue{};
};

/** A progression a redemption credits, and the value its preparation observed. */
struct PreparedProgressionCredit {
    /** Object holding the progression; unreplicated when the grant credits none. */
    build_data::progressions::Scope scope{build_data::progressions::Scope::unreplicated};
    std::uint16_t definitionIndex{};
    std::int32_t previousValue{};
    std::int32_t value{};
};

/** Record claim and all of its item rows committed as one transaction. */
struct PendingRecordRewardGrant {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeProfileItems{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        afterProfileItems{};
    std::array<PreparedRecordReward, kRecordRewardGrantCapacity> rewards{};
    /** Rows still present after the whole batch, each announced once in its change ring. */
    std::array<GrantedRow, kRecordRewardGrantCapacity> rows{};
    std::size_t rowCount{};
    /** Ascending acquisition flags from wrappers which opened without remaining in inventory. */
    std::array<PreparedRewardFlag, kRewardWrapperFlagCapacity> wrapperFlags{};
    std::size_t wrapperFlagCount{};
    /** Record claimed with this grant, already written to the banks, or the unclaimed row. */
    std::uint16_t claimedRecordIndex{kUnclaimedRecordIndex};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t characterIndex{};
    std::size_t beforeProfileItemCount{};
    std::size_t afterProfileItemCount{};
    std::size_t rewardCount{};
    VendorRankRewardClaim vendorReward{};
    /** The rows are existing items a vendor moved, not acquisitions. */
    bool vendorTransfer{};
    /** A held pursuit or wrapper the grant consumes; it leaves the selected character with it. */
    std::uint64_t redeemedInstanceSoid{};
    /** The completion and completion-context values a redemption sets, before its quest step. */
    PreparedQuestStep completionValue{};
    PreparedQuestStep completionContextValue{};
    PreparedQuestStep questStep{};
    PreparedProgressionCredit progression{};
    /** Seasonal XP the commit grants with the rest of this grant. */
    std::int32_t seasonalExperience{};
    /** Seasonal XP total the preparation observed; the grant is refused once it moves. */
    std::int32_t seasonalExperienceBefore{};
    bool prepared{};

    /** @return The flags of the wrappers this grant consumes. */
    [[nodiscard]] std::span<const PreparedRewardFlag> wrapper_flags() const noexcept {
        return std::span<const PreparedRewardFlag>(wrapperFlags).first(wrapperFlagCount);
    }
    /** @return The saved values this grant moves on, in write order; a later one wins a row. */
    [[nodiscard]] std::array<const PreparedQuestStep*, 3> saved_value_moves() const noexcept {
        return {&completionValue, &completionContextValue, &questStep};
    }
};

/** A Postmaster claim: the rows it debits and returns, kept private until its update fits. */
struct PendingPostmasterClaim {
    PendingRecordRewardGrant grant{};
    std::uint16_t vendorIndex{};
    std::uint8_t sourceBucket{};
    bool prepared{};
};

/** Item-instance residents one grant adds, in the order its publication appends them. */
struct GrantedResidents {
    std::array<std::uint64_t, kRecordRewardGrantCapacity> soids{};
    std::size_t count{};
    /** Held or equipped character instances a FIFO eviction discarded. */
    std::array<std::uint64_t, kRecordRewardGrantCapacity> released{};
    std::size_t releasedCount{};
};

/**
 * Lists the new character instances, then the new profile action-source rows, each in after-image
 * order, and the character instances the grant removed. A row an eviction moved, equipped or not,
 * keeps its identity and is neither.
 * @return False when the grant adds or removes more residents than one batch can carry.
 */
[[nodiscard]] bool granted_residents(const PendingRecordRewardGrant& mutation,
                                     GrantedResidents& output) noexcept;

/**
 * @return True when a held character instance carries other per-item values after a transaction
 *         than before it, as a bounty the transaction advanced does. A new instance has no
 *         earlier copy, so it is not edited.
 */
[[nodiscard]] bool item_values_edited(const CharacterState& before,
                                      const CharacterState& after,
                                      std::uint64_t instanceSoid) noexcept;

/**
 * Whether a granted row is a new row of an item with a Collections entry whose acquisition flag
 * this grant first sets. Sunrise keeps a collectible's acquired state in that flag; the client's
 * record writer marks such a row for its list of newly acquired collectibles.
 * @param item The row's item.
 */
[[nodiscard]] bool newly_collected(const PendingRecordRewardGrant& mutation,
                                   const GrantedRow& row,
                                   const build_data::items::Definition& item) noexcept;

/** How one reward preparation ended. */
enum class RewardPreparation : std::uint8_t {
    prepared,
    /** Saved State refuses the reward now; only a later State can change that. */
    deferred,
    /** A catalog is unpublished, or a saved read or allocation failed, so nothing was decided. */
    notReady,
    unresolvable,
};

/** One uncommitted Season reward, the native row it will claim, and the seed of its draw. */
struct PendingSeasonPassReward {
    PendingRecordRewardGrant grant{};
    std::uint32_t sourceDefinitionHash{};
    std::uint64_t seed{};
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

/**
 * Prepared selected-character inventory removal, or a held stack's discard, kept private until
 * its reply and push fit.
 */
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
    /** The removed instance, or a stack's definition and the quantity it held before. */
    account::inventory::Item dismantledItem{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    /** Zero for a stack, which releases no resident. */
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
    /** A payout was credited, or the discarded stack or instance is a profile row. */
    bool profileChanged{};
    /** The removed instance was a profile row, so no character row changed. */
    bool profileResident{};
    /**
     * The consumed item's acquisition flag and the value preparation read. Every held copy was
     * acquired, so consuming one through its delete-on-action records that when the flag is clear.
     */
    PreparedRewardFlag acquiredFlag{};
    bool setsAcquiredFlag{};

    /** @return True when the dismantle changes the account object. */
    [[nodiscard]] bool updates_account() const noexcept {
        return profileChanged || setsAcquiredFlag;
    }
    /** False for an editor removal or a stack, which credit no dismantle materials. */
    bool paysRewards{true};
    bool prepared{};
};

/** One row the caller read and the quantity that replaces it. */
struct QuantityEdit {
    /**
     * The saved array the edit names. It must be the array of the bucket the row occupies, which
     * for a recovered row is its item's recovery bucket.
     */
    GrantedRowArray scope{};
    /** Character expected to be selected. A profile edit is published with this character too. */
    std::uint64_t characterSoid{};
    /** Zero for plain profile rows and character stacks. */
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t mutationSerial{};
    std::int32_t expectedQuantity{};
    /** Zero removes the row without dismantle materials. */
    std::int32_t quantity{};

    bool operator==(const QuantityEdit&) const = default;
};

/** The saved objects one committed edit changed, for the caller's refresh. */
struct AccountEditChange {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t characterIndex{};
    /** A row the account object publishes changed, so that object is refreshed too. */
    bool accountRows{};
    /**
     * The held item whose own values the edit changed, or zero. Those values travel in the item's
     * resident, which is then sent ahead of the character object.
     */
    std::uint64_t itemInstanceSoid{};
};

/**
 * Account unlock rows one socket action may write: a debit for each requirement of its cost set,
 * plus the two ingredient balances a burnt bake debits, the discovery flag a recipe sets or the
 * flag a Chalice upgrade sets.
 */
inline constexpr std::size_t kSocketUnlockWriteCapacity = 8;

/** The account unlock rows one socket action writes, each once, in the order first written. */
struct AccountUnlockWrites {
    std::array<AccountUnlockWrite, kSocketUnlockWriteCapacity> rows{};
    std::size_t count{};

    bool operator==(const AccountUnlockWrites&) const = default;
};

/** Prepared ordinary-socket selection for one selected-character item instance. */
struct PendingSocketPlug {
    /** Exact prepare-time character view used as the commit staleness guard. */
    CharacterState beforeCharacter{};
    /**
     * Canonical after-image. Only the target item's authored socket block differs, and the values
     * and serials of the bounties a bake advanced.
     */
    CharacterState afterCharacter{};
    /** Exact account-wide material balances observed before applying the installed cost set. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeProfileItems{};
    /** Canonical balances after every consuming row of the cost set and any exchange payout. */
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
    /** Rows an exchange credited, which the account's change ring announces. */
    std::array<ProfileStackChange, kProfileStackChangeCapacity> changes{};
    std::size_t changeCount{};
    /** Account values the cost's value-slot rows debit, and flags a bake sets. */
    AccountUnlockWrites unlockWrites{};
    /** Equipment semantic index or dense inventory index, selected by `targetEquipped`. */
    std::size_t itemIndex{};
    std::uint16_t targetDefinitionIndex{};
    /**
     * Plug that lands in the lane. A rolled socket lands a result plug, and an exchange returns its
     * lane to the initial plug.
     */
    std::uint16_t plugDefinitionIndex{};
    /** Plug the Client asked for, which decides the pool check and the material charge. */
    std::uint16_t requestedPlugDefinitionIndex{};
    std::uint16_t materialRequirementSetIndex{0xFFFFU};
    std::uint8_t socketLane{};
    std::uint8_t targetBucketId{};
    std::uint8_t plugBucketId{};
    std::uint8_t materialRequirementCount{};
    /** Server time in Unix seconds the bake's bounty deadlines were compared with. */
    std::int64_t nowSeconds{};
    /** The account object changes: a profile row or an account unlock row. */
    bool profileChanged{};
    bool targetEquipped{};
    bool prepared{};

    /** @return True when a character row took a new serial, so the character object changes. */
    [[nodiscard]] bool updates_character() const noexcept {
        return beforeCharacter.nextInventorySerial != afterCharacter.nextInventorySerial;
    }
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
    /** The purchase's perk walk set an account flag, which only the account image carries. */
    bool perksEarned{};
};

/** Item residents whose authored artifact sockets were cleared by one reset. */
struct ArtifactResetResult {
    std::array<std::uint64_t,
               account::inventory::kEquipmentSlotCount + account::inventory::kCharacterItemCapacity>
        instanceSoids{};
    std::size_t instanceCount{};
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
 * Advances the Store wallet sync generation the account object publishes. The request's platform
 * receipts are not purchase authority, so nothing is granted, charged or read from a wallet.
 * Composes with the caller's investment transaction.
 * @param revision Receives the new generation; zero on failure.
 */
[[nodiscard]] bool synchronize_store_wallet(std::uint32_t& revision) noexcept;

/**
 * Reads the committed Store wallet sync generation.
 * @param revision Receives the generation, or zero when the account never synced.
 */
[[nodiscard]] bool read_store_wallet_sync(std::uint32_t& revision) noexcept;

/**
 * Reads the Synthesizer tier each character publishes in its value block of the account object.
 * @param account Account view the object is encoded from.
 * @param valueRow Receives the block row the tiers belong in, or kUnboundValueRow for none.
 * @param tiers Receives one tier per character slot, zero unless it holds exactly one Synthesizer.
 */
void synthesizer_tier_values(const AccountState& account,
                             std::uint16_t& valueRow,
                             std::array<std::int32_t, kCharacterCapacity>& tiers) noexcept;

/**
 * Moves the selection to one authored character.
 * The Client names its pick only in the select-character request, so this is where a player's
 * choice enters State.
 * @param characterSoid Picked character key, which must name an authored character.
 * @param moved Set when the pick replaced another selection or none; a re-pick leaves it clear.
 * @param perksEarned Set when the pick earned perks for the account flag bank.
 * @return False when no authored character carries that key.
 */
[[nodiscard]] bool
set_selected_character(std::uint64_t characterSoid, bool& moved, bool& perksEarned) noexcept;

/**
 * Adds an unselected character to a free slot with its starter set and unlocks, and saves.
 * @param characterClass Character class: 0 Titan, 1 Hunter, 2 Warlock.
 * @param gender Character gender: 0 male, 1 female.
 * @param race Character race: 0 human, 1 awoken, 2 exo.
 * @param appearanceHeader Appearance header from the request, in record byte order.
 * @param characterSoid Receives the new character's id, zero on failure.
 * @return False when nothing was saved.
 */
[[nodiscard]] bool create_character(std::uint8_t characterClass,
                                    std::uint8_t gender,
                                    std::uint8_t race,
                                    const AppearanceHeader& appearanceHeader,
                                    std::uint64_t& characterSoid) noexcept;

/**
 * Removes one character with its items and slot-keyed rows and saves the account.
 * Deleting the selected character leaves no selection; only a pick selects.
 * @param characterSoid Id of the character to remove.
 * @param selectionMoved Set when the removed character was the selected one.
 * @return False when nothing was saved.
 */
[[nodiscard]] bool delete_character(std::uint64_t characterSoid, bool& selectionMoved) noexcept;

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
 * Prepares moving a selected-character instance or stack quantity from the item's recovery bucket
 * back to its authored bucket.
 * @param vendorIndex Vendor whose installed transfer rule must return this source bucket.
 * @param sourceBucket Bucket the Client named; it must be the item's recovery bucket.
 * @param instanceSoid Instance to move, or zero for a stack.
 * @param definitionIndex Definition row the Client named, checked against the claimed rows.
 * @param quantity Requested quantity; 1 for an instance.
 * @param mutation Gets the checked after-image without changing account State.
 * @param reason Receives the static refusal label.
 * @return True when the rule accepts the bucket and the authored bucket has room.
 */
[[nodiscard]] bool prepare_postmaster_claim(std::uint16_t vendorIndex,
                                            std::uint8_t sourceBucket,
                                            std::uint64_t instanceSoid,
                                            std::uint16_t definitionIndex,
                                            std::int32_t quantity,
                                            PendingPostmasterClaim& mutation,
                                            const char*& reason) noexcept;
/** Rechecks the claim under the transaction lock and commits it; always consumes the mutation. */
[[nodiscard]] bool commit_postmaster_claim(PendingPostmasterClaim& mutation) noexcept;

/**
 * Prepares one installed equippable definition as a new selected-character inventory instance.
 *
 * Native-default sockets, a unique runtime SOID, and the selected character's current item level
 * are used. Full loadout resolution is the authoritative bucket-capacity check.
 *
 * @param collectibleIndex Collections row that owns the item, or kNoCollectibleIndex.
 * @param definitionHash Installed item definition requested by the Client.
 * @param price A vendor sale row's cost entries, spent in place of the collectible's material
 *        set; absent for a Collections pull, which pays with the collectible's materials.
 * @param nowSeconds Server time in Unix seconds; a bounty's lifetime runs from it.
 * @param mutation Gets a checked after-image without changing account State.
 * @return True when the item and every existing loadout row resolve with one free native row.
 */
[[nodiscard]] bool
prepare_item_acquisition(std::uint16_t collectibleIndex,
                         std::uint32_t definitionHash,
                         std::optional<std::span<const build_data::vendors::SaleCost>> price,
                         std::int64_t nowSeconds,
                         PendingItemAcquisition& mutation) noexcept;

/** Atomically commits one prepared reward grant and its durable Season claim. */
[[nodiscard]] bool commit_season_pass_reward(PendingSeasonPassReward& mutation) noexcept;
/**
 * @return True once every catalog prepare_item_reward reads has published, or the reward graph
 *         was refused; until then a not-ready answer can change without any saved write.
 */
[[nodiscard]] bool catalogs_settled() noexcept;
/**
 * Resolves an installed item or wrapper for the selected character.
 * @param seed Caller-drawn entropy for any wrapper draws.
 * @param nowSeconds Server time in Unix seconds; a granted bounty's lifetime runs from it.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] RewardPreparation prepare_item_reward(std::uint16_t itemIndex,
                                                    std::uint32_t quantity,
                                                    std::uint64_t seed,
                                                    std::int64_t nowSeconds,
                                                    PendingRecordRewardGrant& mutation,
                                                    const char** refusal = nullptr) noexcept;
/**
 * @return True when a vendor row selling this item grants through the reward graph: the graph
 *         records the item's acquisition in a flag, which a Store row's ownership gate reads, or
 *         the item lands in a character FIFO bucket that discards what it evicts, as a Dawning
 *         delivery does.
 */
[[nodiscard]] bool vendor_reward_item(std::uint16_t itemIndex) noexcept;
/**
 * Evaluates one sale row's availability conditions against the selected character's saved
 * unlocks. A row with none is always available.
 * @param row The resolved sale row.
 * @param available Receives whether the row may be bought.
 * @param refusal Receives a static reason when the conditions cannot be evaluated.
 * @return False when the conditions did not read, the saved state did not, or they did not
 *         evaluate.
 */
[[nodiscard]] bool vendor_row_available(const build_data::vendors::SaleRow& row,
                                        bool& available,
                                        const char** refusal = nullptr) noexcept;
/**
 * Spends a vendor row's price and resolves one of its item for the selected character, in one
 * after-image.
 * @param price The row's cost entries.
 * @param seed Caller-drawn entropy for any wrapper draws.
 * @param nowSeconds Server time in Unix seconds; a granted bounty's lifetime runs from it.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] RewardPreparation
prepare_vendor_reward(std::uint16_t itemIndex,
                      std::span<const build_data::vendors::SaleCost> price,
                      std::uint64_t seed,
                      std::int64_t nowSeconds,
                      PendingRecordRewardGrant& mutation,
                      const char** refusal = nullptr) noexcept;
/**
 * Opens a stored wrapper the selected character holds, such as an engram: the held unit leaves and
 * its draws land in one after-image.
 * @param instanceSoid The held wrapper instance.
 * @param seed Caller-drawn entropy for the draws.
 * @param nowSeconds Server time in Unix seconds; a granted bounty's lifetime runs from it.
 * @param refusal Receives a static reason on failure.
 * @return Deferred when the character holds no such unit or has no room; unresolvable when the
 *         item is not a stored wrapper or its draw is refused.
 */
[[nodiscard]] RewardPreparation prepare_wrapper_opening(std::uint64_t instanceSoid,
                                                        std::uint64_t seed,
                                                        std::int64_t nowSeconds,
                                                        PendingRecordRewardGrant& mutation,
                                                        const char** refusal = nullptr) noexcept;
/**
 * Prepares an eligible native pass row without writing its claim or acquisition flags.
 * @param seed Caller-drawn entropy; the commit replays the same draw from it.
 * @param nowSeconds Server time in Unix seconds; a granted bounty's lifetime runs from it.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool prepare_season_pass_reward(std::uint16_t rewardIndex,
                                              std::uint64_t seed,
                                              std::int64_t nowSeconds,
                                              PendingSeasonPassReward& mutation,
                                              const char** refusal = nullptr) noexcept;
/** Applies a prepared grant's acquisition flags to the saved banks; false if one moved. */
[[nodiscard]] bool preview_reward_unlocks(const PendingRecordRewardGrant& mutation,
                                          unlocks::Table& after) noexcept;

/**
 * Atomically commits one prepared Triumph reward, its durable record claim and its seasonal XP.
 * @param artifactBonusMoved Set when the grant's XP changed the artifact Power bonus.
 */
[[nodiscard]] bool commit_record_reward(PendingRecordRewardGrant& mutation,
                                        bool* artifactBonusMoved = nullptr) noexcept;

/**
 * Prepares all direct reward rows over one shared account after-image.
 * @param rewards Item rows the record grants.
 * @param claimedRecordIndex Record already claimed in the banks, or kUnclaimedRecordIndex.
 * @param nowSeconds Server time in Unix seconds; a granted bounty's lifetime runs from it.
 * @param mutation Receives the prepared grant.
 * @return Prepared when every row fits the account after-image.
 */
[[nodiscard]] RewardPreparation
prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                            std::uint16_t claimedRecordIndex,
                            std::int64_t nowSeconds,
                            PendingRecordRewardGrant& mutation,
                            const char** refusal = nullptr) noexcept;

/** Builds the full account after-image while a record reward remains current. */
[[nodiscard]] bool preview_record_reward_grant(const PendingRecordRewardGrant& mutation,
                                               AccountState& after) noexcept;

/** How preparing one pursuit redemption ended. */
enum class PursuitRedemption : std::uint8_t {
    prepared,
    /**
     * The pursuit is incomplete, expired or suppressed, its progress cannot be read, or it is a
     * quest step whose progress is not held by the item; it is not redeemed, and the request is an
     * ordinary removal.
     */
    notRedeemable,
    /** A catalog is unpublished, or a saved read or allocation failed, so nothing was decided. */
    notReady,
    /** The redemption cannot be judged or prepared; nothing may remove the pursuit. */
    refused,
};

/**
 * Prepares redeeming one completed pursuit the selected character holds: its completion values are
 * set to one, the pursuit leaves inventory unless it names a completion value, its quest set moves
 * to the next step, and the grant places the next step's item or, at the last step, what the
 * reward rows the pursuit shows pay: items, rank points and seasonal XP. A pursuit whose
 * completion value is already set is not redeemed again.
 * @param instanceSoid Held pursuit instance.
 * @param definitionHash Definition the Client named, which the instance must carry.
 * @param nowSeconds Server time in Unix seconds, which a bounty's deadline is compared with.
 * @param seed Caller-drawn entropy for any wrapper a reward row names.
 * @param mutation Receives the prepared grant.
 * @param refusal Receives a static reason when the result is not prepared.
 */
[[nodiscard]] PursuitRedemption prepare_pursuit_redemption(std::uint64_t instanceSoid,
                                                           std::uint32_t definitionHash,
                                                           std::int64_t nowSeconds,
                                                           std::uint64_t seed,
                                                           PendingRecordRewardGrant& mutation,
                                                           const char** refusal = nullptr) noexcept;

/**
 * Prepares redeeming the next held pursuit whose contract completes automatically, judging each
 * one as a redemption request would.
 * @param nowSeconds Server time in Unix seconds, which a bounty's deadline is compared with.
 * @param seed Caller-drawn entropy for any wrapper a reward row names.
 * @param cursor Selected character's inventory row the scan starts at; moves past each row read.
 * @param mutation Receives the prepared grant.
 * @param refusal Receives a static reason when no candidate could be judged.
 * @return Prepared for the first candidate that redeems. Otherwise notReady when any candidate
 *         could not be judged, refused when any was refused, and notRedeemable when none redeems.
 */
[[nodiscard]] PursuitRedemption
prepare_next_automatic_pursuit(std::int64_t nowSeconds,
                               std::uint64_t seed,
                               std::size_t& cursor,
                               PendingRecordRewardGrant& mutation,
                               const char** refusal = nullptr) noexcept;

/** The bubble a kill happened in. A bubble's name is unique only inside its scenario. */
struct KillArea {
    /** Package tag of the scenario the activity's generated world binds. */
    std::uint32_t scenarioTag{};
    /** Name hash of the bubble the killer's client has instantiated. */
    std::uint32_t bubbleNameHash{};
};

/** Names the bubble a kill happened in; absent when the region or its world does not bind. */
using KillAreaResolver = std::optional<KillArea> (*)(void* context) noexcept;

/** The facts one kill incident reports, as kill objectives read them. */
struct KillObservation {
    /** Player key of the killer; a character's player key is its SOID. */
    std::uint64_t killerKey{};
    /** Zero unless the victim was a player. */
    std::uint64_t victimKey{};
    /** Name hash of the victim's actor class. */
    std::uint32_t victimClassHash{};
    /** The victim record's combatant flag. */
    bool victimCombatant{};
    /** Combat labels of the killing source, the killing actor and the victim. */
    std::span<const std::uint32_t> sourceLabels{};
    std::span<const std::uint32_t> actorLabels{};
    std::span<const std::uint32_t> victimLabels{};
    /** Equipment slot of the killing source; absent when the incident omits it. */
    std::optional<std::int8_t> equipmentSlot{};
    /** Ability selector of the killing source; -1 names no ability. */
    std::optional<std::int8_t> abilitySelector{};
    /** Damage type of the final blow; -1 when the incident names none. */
    std::int8_t damageType{-1};
    /** Package name of the destination the kill happened in. */
    std::string_view destination{};
    /** Called at most once, and only when a held rule the kill otherwise meets names an area. */
    KillAreaResolver resolveArea{};
    /** Passed to resolveArea untouched. */
    void* areaContext{};
};

/** How one kill changed the selected character's held bounties. */
enum class KillProgress : std::uint8_t {
    /** At least one objective lane or grenade Triumph advanced and the save holds it. */
    advanced,
    /** No open bounty objective the kill meets and no Triumph counts it. */
    unchanged,
    /** The kill is not the selected character's enemy kill, or the save refused the change. */
    refused,
    /** A catalog is unpublished, or a saved read or allocation failed, so nothing was decided. */
    notReady,
};

/** The held bounties whose own values one kill changed, in inventory order. */
struct KillAdvancedItems {
    /** One slot for every unequipped row, so a kill that advances every held item fits. */
    std::array<std::uint64_t, account::inventory::kCharacterItemCapacity> instanceSoids{};
    std::size_t count{};
};

/** What the receipt of one kill logs beside its progress, and what an advanced kill changed. */
struct KillReport {
    /** Static reason nothing advanced; null when the kill advanced. */
    const char* reason{};
    /** Held items a kill rule names, as last counted; zero until the held items are read. */
    std::size_t ruledItems{};
    /** Bounties the kill advanced; empty unless the kill advanced. */
    KillAdvancedItems advancedItems{};
    /** The kill moved a grenade Triumph, whose progress lives in the account's unlock banks. */
    bool triumphAdvanced{};
};

/**
 * Advances, by one, every open objective lane of the selected character's held bounties that the
 * kill meets, and the grenade interval Triumph it counts, in one save transaction. The victim is
 * judged and the held items are read only while something is left to advance.
 * @param nowSeconds Server time in Unix seconds; an expired bounty does not advance.
 * @param report Receives why nothing advanced and how many held items a rule names, or which
 *        bounties advanced and whether a Triumph moved.
 */
[[nodiscard]] KillProgress advance_kill_objectives(const KillObservation& kill,
                                                   std::int64_t nowSeconds,
                                                   KillReport& report) noexcept;

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
 * @param collectibleIndex Collections row that owns the item, or kNoCollectibleIndex.
 * @param definitionHash Installed stackable definition requested by the Client.
 * @param price A vendor sale row's cost entries, spent in place of the collectible's material
 *        set; absent for a Collections pull, which pays with the collectible's materials.
 * @param mutation Gets the checked profile before/after images without changing account State.
 * @return True when the definition belongs to the main profile array and one unit fits.
 */
[[nodiscard]] bool prepare_profile_item_acquisition(
    std::uint16_t collectibleIndex,
    std::uint32_t definitionHash,
    std::optional<std::span<const build_data::vendors::SaleCost>> price,
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

/**
 * Prepares the discard of a stack the selected character or the profile holds: the whole stack
 * when the item's action consumes it entire, otherwise one unit.
 * A stack has no instance, so its row is the first that holds exactly the quantity the Client
 * shows, searching the character's stacks before the profile. The last unit removes the row.
 * Nothing is credited.
 * @param definitionHash Stacked item the Client named.
 * @param heldQuantity Quantity the Client shows for that row.
 * @param mutation Gets checked before/after images without changing account State.
 * @return True when such a row is held.
 */
[[nodiscard]] bool prepare_stack_discard(std::uint32_t definitionHash,
                                         std::int32_t heldQuantity,
                                         PendingItemDismantle& mutation) noexcept;

/**
 * Prepares the same removal as prepare_item_dismantle, but only for the exact instance the caller
 * read, and without crediting dismantle materials.
 * @param characterSoid Character expected to be selected.
 * @param instanceSoid Unequipped item instance to remove.
 * @param definitionHash Definition the instance is expected to carry.
 * @param mutationSerial Serial the instance is expected to carry.
 * @param quantity Quantity the instance is expected to carry.
 * @param mutation Gets checked before/after images without changing account State.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool prepare_item_removal(std::uint64_t characterSoid,
                                        std::uint64_t instanceSoid,
                                        std::uint32_t definitionHash,
                                        std::int32_t mutationSerial,
                                        std::int32_t quantity,
                                        PendingItemDismantle& mutation,
                                        const char** refusal = nullptr) noexcept;

/**
 * Checks a quantity edit without reading saved state: the quantity is not negative and the item
 * is installed. set_quantity applies the same check first.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool check_quantity_edit(const QuantityEdit& edit,
                                       const char** refusal = nullptr) noexcept;

/**
 * Replaces the quantity of one unequipped row that still matches what the caller read. It runs
 * in its own savepoint, so an enclosing store transaction can still roll it back. Instanced rows
 * are never removed here: an unequipped item is removed through prepare_item_removal, and
 * removing a profile action source is refused.
 * @param edit Expected row identity, quantity and replacement.
 * @param change Receives the changed objects on success.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool set_quantity(const QuantityEdit& edit,
                                AccountEditChange& change,
                                const char** refusal = nullptr) noexcept;

/** Builds the exact account after-image while a prepared dismantle remains current. */
[[nodiscard]] bool preview_item_dismantle(const PendingItemDismantle& mutation,
                                          AccountState& after) noexcept;

/**
 * Builds the account unlocks a prepared dismantle publishes, with any acquisition flag it sets.
 * @param after Receives the selected character's saved banks after the dismantle.
 * @return False when the banks do not read or the flag changed since preparation.
 */
[[nodiscard]] bool preview_dismantle_unlocks(const PendingItemDismantle& mutation,
                                             unlocks::Table& after) noexcept;

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
 * A crafting container's plug can be an exchange, which also changes other lanes, profile rows,
 * account unlock rows and held bounties; the mutation carries them all.
 * @param targetInstanceSoid Selected-character item-instance key named by the Client.
 * @param socketLane Zero-based ordinary socket lane.
 * @param plugDefinitionIndex Installed plug-definition row selected by the Client.
 * @param nowSeconds Server time in Unix seconds, which a bake's bounty deadlines are compared with.
 * @param mutation Gets the checked before/after images without changing account State.
 * @return True when ownership, item detail, lane, plug compatibility, and both loadouts validate.
 */
[[nodiscard]] bool prepare_socket_plug(std::uint64_t targetInstanceSoid,
                                       std::uint8_t socketLane,
                                       std::uint16_t plugDefinitionIndex,
                                       std::int64_t nowSeconds,
                                       PendingSocketPlug& mutation) noexcept;

/**
 * Prepares one ordinary-socket selection for the item instance opcode 1901 names.
 * @param targetInstanceSoid Whole item-instance identity carried by opcode 1901.
 * @param requestedSocketLane Native socket action lane; compatibility resolves the physical lane.
 * @param plugDefinitionIndex Installed plug-definition row selected by the Client.
 * @param nowSeconds Server time in Unix seconds, which a bake's bounty deadlines are compared with.
 * @param mutation Gets the checked before/after images without changing account State.
 * @return True when one item matches, the plug resolves to that lane, and the transition is valid.
 */
[[nodiscard]] bool prepare_equipped_socket_plug(std::uint64_t targetInstanceSoid,
                                                std::uint8_t requestedSocketLane,
                                                std::uint16_t plugDefinitionIndex,
                                                std::int64_t nowSeconds,
                                                PendingSocketPlug& mutation) noexcept;

/**
 * Produces the complete uncommitted account after-image for a prepared socket transaction.
 * @param afterUnlocks Receives the account unlocks with the action's writes applied.
 */
[[nodiscard]] bool preview_socket_plug(const PendingSocketPlug& mutation,
                                       AccountState& after,
                                       unlocks::Table& afterUnlocks) noexcept;

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

/** @return The bound account under its scope's visibility; before public profile arrival, only
 * its enrolled primary SOID is populated. Invalid ownership returns an empty account. */
[[nodiscard]] AccountState bound_account_snapshot() noexcept;
/** Publishes the actual local BAP listener port before any client signs on. */
void publish_bap_port(std::uint16_t port) noexcept;

/**
 * Reads the account and character keys without copying inventory or settings.
 * @return False when the saved account cannot be read.
 */
[[nodiscard]] bool account_identity(AccountIdentity& output) noexcept;

/**
 * Copies the evaluated content state and adds build-derived catalyst completion overrides.
 * @param output Receives one complete Family-5 snapshot on success.
 * @return False when the fixed override banks cannot hold the complete state.
 */
[[nodiscard]] bool investment_snapshot(InvestmentState& output) noexcept;

/**
 * Says whether Family-5 owes a snapshot: the items the profile holds supply other presence flags
 * than the last snapshot set.
 */
[[nodiscard]] bool presence_flags_pending() noexcept;

/** Seasonal artifact item definition, whose equipped row carries the Power bonus stat. */
inline constexpr std::uint32_t kSeasonalArtifactItemHash = 0x613A3DA6U;

/** Native progression row carrying the seasonal artifact Power ladder. */
inline constexpr std::uint16_t kArtifactPowerProgressionIndex = 38;
/** Native progression row carrying the seasonal artifact unlock-point ladder. */
inline constexpr std::uint16_t kArtifactUnlockProgressionIndex = 39;

/** @return Seasonal XP published in the account progression bank. */
[[nodiscard]] std::int32_t seasonal_experience() noexcept;

/**
 * Publishes every seasonal value the seeded XP and artifact ownership imply, and walks the perks
 * the selected character has earned.
 * @param perksEarned Set when the walk set a flag in the account bank.
 * @return False when a write failed; nothing is committed then.
 */
[[nodiscard]] bool seed_seasonal_progression(bool& perksEarned) noexcept;

/** @return True when the pass, reward and item rows the perk walk reads are in State. */
[[nodiscard]] bool season_perks_walkable() noexcept;

/**
 * Sets the flags of every perk the selected character has earned, in one save transaction.
 * @param perksEarned Set when the walk set a flag in the account bank.
 * @return False when the save could not be read or written; nothing is committed then.
 */
[[nodiscard]] bool grant_earned_season_perks(bool& perksEarned) noexcept;

/** @return One-based Season of Arrivals rank the published XP earns. */
[[nodiscard]] std::uint16_t seasonal_rank() noexcept;

/** @return Account-wide Power bonus published by the seasonal artifact. */
[[nodiscard]] std::uint16_t artifact_power_bonus() noexcept;

/**
 * Adds base XP to the seasonal lanes and republishes every value derived from the total,
 * including earned-perk flags when the grant reaches a new rank.
 * The caller must publish the whole account image, since that carries those flags: after the
 * grant, or before it from the image preview_reward_unlocks builds for a grant that pays XP.
 * @param amount Positive XP to grant.
 * @return False when the amount is not positive or the total would overflow.
 */
[[nodiscard]] bool grant_seasonal_experience(std::int32_t amount) noexcept;

/**
 * Replaces the published XP total, provided it still equals expected, and republishes every
 * value derived from it. An increase grants newly earned perks. A decrease leaves earned and
 * claimed flags set.
 * @param expected XP total the caller read.
 * @param replacement New total. A decrease must still earn the artifact points any character has
 * spent.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool replace_seasonal_experience(std::int32_t expected,
                                               std::int32_t replacement,
                                               const char** refusal = nullptr) noexcept;

/**
 * Checks a replacement XP total without reading saved state. replace_seasonal_experience runs
 * this check first.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool check_seasonal_experience(std::int32_t replacement,
                                             const char** refusal = nullptr) noexcept;

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

/**
 * Clears one claimed row's claim flag. Granted items and acquisition flags stay.
 * Rows the earned-perk walk grants are refused, because that walk owns their claim flag.
 * @param rewardIndex Native reward-array index.
 * @param itemHash Item the row is expected to grant.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool reset_season_pass_reward(std::uint16_t rewardIndex,
                                            std::uint32_t itemHash,
                                            const char** refusal = nullptr) noexcept;

/**
 * Checks that a native pass row exists, still grants the expected item and is not one the
 * earned-perk walk grants, without reading saved state. reset_season_pass_reward applies the same
 * check, and an editor claim is checked the same way. Refuses with `not_ready` while the pass,
 * reward or item catalog is unpublished.
 * @param rewardIndex Native reward-array index.
 * @param itemHash Item the row is expected to grant.
 * @param refusal Receives a static reason on failure.
 */
[[nodiscard]] bool check_season_pass_reward(std::uint16_t rewardIndex,
                                            std::uint32_t itemHash,
                                            const char** refusal = nullptr) noexcept;

/** @return Purchased artifact sale rows, one bit per row. */
[[nodiscard]] std::uint32_t artifact_mod_mask() noexcept;

/**
 * Replaces the exact published artifact mask, refusing when it already moved, then walks the
 * earned perks. The caller publishes the account image or rolls the change back.
 */
[[nodiscard]] bool replace_artifact_mod_mask(std::uint32_t expected,
                                             std::uint32_t replacement) noexcept;

/** Writes one affordable artifact purchase, walks the earned perks, and keeps the before-image. */
[[nodiscard]] bool prepare_artifact_mod_unlock(std::uint16_t saleIndex,
                                               PendingArtifactPurchase& mutation) noexcept;

/** Keeps a prepared artifact purchase only while its character and mask are still current. */
[[nodiscard]] bool commit_artifact_mod_unlock(PendingArtifactPurchase& mutation) noexcept;

/** Charges Glimmer, removes artifact mods, and refunds every spent unlock point. */
[[nodiscard]] bool reset_artifact(std::int32_t glimmerCost, ArtifactResetResult& result) noexcept;

} // namespace sunrise::state
