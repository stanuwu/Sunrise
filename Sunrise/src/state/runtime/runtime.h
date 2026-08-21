#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "state.h"

namespace sunrise::state {

/**
 * Assigns runtime SOIDs only to installed profile mod/shader rows which are socket action sources.
 * Currency, material, and consumable profile rows remain canonically non-instanced.
 */
[[nodiscard]] bool ensure_profile_item_identities() noexcept;

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
    /** Exact profile material view observed before and after charging the native requirement set.
     */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        beforeProfileItems{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        afterProfileItems{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t acquiredInstanceSoid{};
    std::uint32_t acquiredDefinitionHash{};
    std::uint32_t materialRequirementSetHash{};
    std::uint32_t expectedNextInventorySerial{};
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
    bool prepared{};
};

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
    /** True only for installed profile mod/shader rows materialized as Family-4 residents. */
    bool actionSource{};
    bool appended{};
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

/**
 * Loads cached build data and generates secrets with Sunrise's authored activity defaults.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @return True when the cached data passes its checks and every secret is generated.
 */
[[nodiscard]] bool initialize(void* module = nullptr,
                              const AccountState& initialAccount = {}) noexcept;

/**
 * Loads cached build data and publishes fixed activity defaults in one step.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @param activityDefaults Complete local fallback policy from immutable Core settings.
 * @return True when account, defaults, cached data, and generated secrets are valid.
 */
[[nodiscard]] bool
initialize(void* module,
           const AccountState& initialAccount,
           const activity::defaults::ActivityDefaults& activityDefaults) noexcept;

/** Securely clears State, including activity destinations and matchmaking descriptors. */
void shutdown() noexcept;

/** @return Immutable generated SignOn session fields. */
[[nodiscard]] const SignOnState& sign_on() noexcept;

[[nodiscard]] bool publish_bootstrap_token(std::span<const std::byte> token) noexcept;

/** @return Immutable generated BAP session fields. */
[[nodiscard]] const BapState& bap() noexcept;

/**
 * Stores the active nonzero account key when the account remains complete.
 * @param primarySoid Account key selected by the local Client.
 * @return False when the key or resulting account State is invalid.
 */
[[nodiscard]] bool set_primary_soid(std::uint64_t primarySoid) noexcept;

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
 * @return True
 * when the equip or unequip commits atomically and leaves the whole account valid.
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

/** Builds the exact full-account after-image while a prepared item pull remains current. */
[[nodiscard]] bool preview_item_acquisition(const PendingItemAcquisition& mutation,
                                            AccountState& after) noexcept;

/**
 * Commits a prepared inventory insertion only while its selected character, existing loadout,
 * and next inventory serial still match the prepare-time view.
 *
 * @param mutation Prepared mutation, always cleared before this function returns.
 * @return True when the insertion commits atomically and leaves the whole account valid.
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

/** @return A copy of the active account state, read under the lock. */
[[nodiscard]] AccountState account_snapshot() noexcept;

/**
 * Copies the evaluated content state and adds build-derived catalyst completion overrides.
 * @param output Receives one complete Family-5 snapshot on success.
 * @return False when the fixed override banks cannot hold the complete state.
 */
[[nodiscard]] bool investment_snapshot(InvestmentState& output) noexcept;

} // namespace sunrise::state
