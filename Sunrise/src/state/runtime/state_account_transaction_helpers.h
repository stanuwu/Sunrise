#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "runtime.h"

namespace sunrise::state::runtime::detail {

/** Clears a consumed mutation on every commit exit without copying its full snapshot. */
template <typename Pending> class PendingConsumption final {
public:
    static_assert(std::is_nothrow_default_constructible_v<Pending>);
    static_assert(std::is_nothrow_destructible_v<Pending>);

    explicit PendingConsumption(Pending& pending) noexcept : pending_(pending) {}
    PendingConsumption(const PendingConsumption&) = delete;
    PendingConsumption(PendingConsumption&&) = delete;
    PendingConsumption& operator=(const PendingConsumption&) = delete;
    PendingConsumption& operator=(PendingConsumption&&) = delete;
    ~PendingConsumption() noexcept {
        std::destroy_at(std::addressof(pending_));
        std::construct_at(std::addressof(pending_));
    }

private:
    Pending& pending_;
};

/** Where one resolved loadout places an instance, and the serial it published there. */
struct ResolvedPosition {
    std::uint16_t inventoryRow{};
    std::uint8_t equipmentSlot{};
    bool equipped{};
    std::int32_t mutationSerial{};
};

/** Stable location of one character-owned item inside authored State. */
struct CharacterItemLocation {
    std::size_t index{};
    bool equipped{};
};

void report_dismantle(std::string_view stage,
                      std::string_view result,
                      std::string_view reason,
                      std::uint32_t definitionHash,
                      std::uint64_t characterSoid,
                      std::uint64_t instanceSoid,
                      std::size_t inventoryIndex,
                      std::uint16_t inventoryRow,
                      std::uint8_t equipmentSlot,
                      std::size_t movedItemCount,
                      std::uint32_t nextInventorySerial) noexcept;
void report_socket_plug(std::string_view stage,
                        std::string_view result,
                        std::string_view reason,
                        std::uint64_t characterSoid,
                        std::uint64_t targetInstanceSoid,
                        std::uint16_t targetDefinitionIndex,
                        std::uint8_t socketLane,
                        std::uint16_t plugDefinitionIndex,
                        std::uint8_t targetBucketId,
                        std::uint8_t plugBucketId,
                        bool targetEquipped,
                        std::size_t itemIndex) noexcept;
void report_item_state(std::string_view stage,
                       std::string_view result,
                       std::string_view reason,
                       std::uint64_t characterSoid,
                       std::uint64_t instanceSoid,
                       std::uint16_t definitionIndex,
                       std::uint32_t beforeFlags,
                       std::uint32_t afterFlags,
                       bool equipped,
                       std::size_t itemIndex) noexcept;

[[nodiscard]] bool
same_profile_inventory(const AccountState& account,
                       const std::array<account::inventory::ProfileItem,
                                        account::inventory::kProfileItemCapacity>& expected,
                       std::size_t expectedCount) noexcept;
[[nodiscard]] bool same_profile_views(
    const std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>&
        left,
    std::size_t leftCount,
    const std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>&
        right,
    std::size_t rightCount) noexcept;
[[nodiscard]] bool valid_profile_inventory(const AccountState& account) noexcept;
[[nodiscard]] bool
apply_collection_materials(const AccountState& before,
                           const build_data::collectibles::Definition& collectible,
                           AccountState& after,
                           bool& changed) noexcept;
[[nodiscard]] bool
valid_profile_mutation_shape(const PendingProfileItemAcquisition& mutation) noexcept;
[[nodiscard]] bool materialize_profile_acquisition(const AccountState& current,
                                                   const PendingProfileItemAcquisition& mutation,
                                                   AccountState& after) noexcept;

/** What paid for one grant: a Collections row with its material cost, or nothing. */
struct GrantSource {
    std::uint32_t materialRequirementSetHash{};
    std::uint16_t collectibleIndex{};
    std::uint8_t materialRequirementCount{};
    bool direct{};
};

/** @return The selected character's index, or the character count when none is selected. */
[[nodiscard]] std::size_t selected_character_index(const AccountState& account) noexcept;
/**
 * Hold investment::store::g_mutex while capturing inventory and quest state together.
 * @param account State before any acquisition charge.
 * @param chargedAccount State after the prepared material charge.
 * @param definitionHash Item definition to grant.
 * @param profileChanged Whether the charge changed profile inventory.
 * @param source Grant identity and material requirements for commit checks.
 * @param mutation Receives a pending grant; use only on success.
 * @return False when the item, inventory, mapping, or saved quest state is invalid.
 */
[[nodiscard]] bool finalize_item_acquisition(const AccountState& account,
                                             const AccountState& chargedAccount,
                                             std::uint32_t definitionHash,
                                             bool profileChanged,
                                             const GrantSource& source,
                                             PendingItemAcquisition& mutation) noexcept;
/**
 * Stages the common profile-stack insertion path.
 * @param chargedAccount Account after any material cost, or account itself when nothing is charged.
 * @param actionSource True when the stack carries a resident instance soid.
 * @return False when the quantity does not fit the stack or the after-image is not canonical.
 */
[[nodiscard]] bool
finalize_profile_item_acquisition(const AccountState& account,
                                  const AccountState& chargedAccount,
                                  std::uint32_t definitionHash,
                                  const build_data::items::details::Definition& detail,
                                  bool actionSource,
                                  std::int32_t quantity,
                                  const GrantSource& source,
                                  PendingProfileItemAcquisition& mutation) noexcept;
/**
 * Hold investment::store::g_mutex; the selected character and saved state must still match.
 * @param current Current account from the locked save view.
 * @param mutation Prepared inventory insertion and prior quest state.
 * @param after Receives the candidate account; use only on success.
 * @return False for stale state or an invalid resulting inventory.
 */
[[nodiscard]] bool materialize_item_acquisition(const AccountState& current,
                                                const PendingItemAcquisition& mutation,
                                                AccountState& after) noexcept;
/**
 * Rebuilds one package from installed policy and rejects any altered after-image.
 * @return False when the account moved or the rebuilt character differs from the mutation.
 */
[[nodiscard]] bool materialize_direct_item_bundle(const AccountState& current,
                                                  const PendingDirectItemBundle& mutation,
                                                  AccountState& after) noexcept;
[[nodiscard]] bool native_equipment_slot(const account::inventory::Item& item,
                                         std::uint8_t& slot) noexcept;
[[nodiscard]] bool semantic_equipment_slot(std::uint8_t nativeSlot,
                                           std::size_t& semanticIndex) noexcept;
[[nodiscard]] bool
find_resolved_position(const middleware::datagen::family4::loadout::ResolvedLoadout& loadout,
                       std::uint64_t instanceSoid,
                       ResolvedPosition& position) noexcept;
[[nodiscard]] bool finalize_equipment_transition(
    const AccountState& account,
    std::size_t characterIndex,
    std::uint64_t requestedInstanceSoid,
    EquipmentMutationKind kind,
    std::uint8_t expectedNativeSlot,
    const middleware::datagen::family4::loadout::ResolvedLoadout& beforeLoadout,
    CharacterState& after,
    std::size_t& movedItemCount) noexcept;
[[nodiscard]] bool same_character(const CharacterState& left, const CharacterState& right) noexcept;
/**
 * @param pinnedPlugHash For a rolled socket's apply or re-roll, the result plug an earlier
 *        staging rolled, so a re-staging reproduces the same after-image; 0 rolls afresh.
 */
[[nodiscard]] bool stage_socket_plug(const AccountState& snapshot,
                                     std::size_t characterIndex,
                                     std::uint64_t targetInstanceSoid,
                                     std::uint8_t socketLane,
                                     std::uint16_t plugDefinitionIndex,
                                     PendingSocketPlug& mutation,
                                     std::uint32_t pinnedPlugHash = 0) noexcept;
[[nodiscard]] bool stage_item_state(const AccountState& snapshot,
                                    std::size_t characterIndex,
                                    std::uint64_t targetInstanceSoid,
                                    std::uint16_t targetDefinitionIndex,
                                    std::uint32_t flags,
                                    PendingItemState& mutation) noexcept;
[[nodiscard]] bool stage_subclass_selection(const AccountState& snapshot,
                                            std::size_t characterIndex,
                                            std::uint64_t subclassInstanceSoid,
                                            std::uint8_t requestedEntry,
                                            PendingSubclassSelection& mutation) noexcept;
[[nodiscard]] bool next_item_instance_soid(const AccountState& account,
                                           std::uint64_t& output) noexcept;
[[nodiscard]] bool next_profile_item_instance_soid(const AccountState& account,
                                                   std::uint64_t& output) noexcept;
[[nodiscard]] bool account_owns_soid(const AccountState& account, std::uint64_t soid) noexcept;
[[nodiscard]] std::int32_t acquisition_level(const CharacterState& character) noexcept;
[[nodiscard]] bool stage_item_dismantle(const AccountState& account,
                                        std::size_t characterIndex,
                                        std::uint64_t instanceSoid,
                                        PendingItemDismantle& mutation) noexcept;
[[nodiscard]] bool same_dismantle_transition(const PendingItemDismantle& left,
                                             const PendingItemDismantle& right) noexcept;
[[nodiscard]] bool materialize_item_dismantle(const AccountState& current,
                                              const PendingItemDismantle& mutation,
                                              AccountState& after) noexcept;
[[nodiscard]] bool identity_uses_soid(const AccountState& account, std::uint64_t soid) noexcept;
[[nodiscard]] bool holds_plug_source(const AccountState& account,
                                     std::uint32_t definitionHash) noexcept;
[[nodiscard]] bool spend_plug_source(AccountState& account, std::uint32_t definitionHash) noexcept;
[[nodiscard]] bool
apply_action_materials(const AccountState& before,
                       const build_data::material_requirements::Definition& definition,
                       AccountState& after,
                       bool& changed) noexcept;
[[nodiscard]] bool inventory_bucket_id(const account::inventory::Item& item,
                                       std::uint8_t& bucketId) noexcept;
[[nodiscard]] bool same_position(const ResolvedPosition& left,
                                 const ResolvedPosition& right) noexcept;
[[nodiscard]] bool same_stationary_item(const account::inventory::Item& left,
                                        const account::inventory::Item& right) noexcept;
[[nodiscard]] bool find_character_item_location(const CharacterState& character,
                                                std::uint64_t instanceSoid,
                                                CharacterItemLocation& location) noexcept;
[[nodiscard]] const account::inventory::Item*
character_item_at(const CharacterState& character, const CharacterItemLocation& location) noexcept;
[[nodiscard]] account::inventory::Item*
character_item_at(CharacterState& character, const CharacterItemLocation& location) noexcept;
[[nodiscard]] bool
find_unequipped_row(const middleware::datagen::family4::loadout::ResolvedLoadout& loadout,
                    std::uint64_t instanceSoid,
                    std::uint16_t& inventoryRow,
                    std::uint8_t& equipmentSlot) noexcept;
[[nodiscard]] bool
loadout_contains(const middleware::datagen::family4::loadout::ResolvedLoadout& loadout,
                 std::uint64_t instanceSoid) noexcept;

} // namespace sunrise::state::runtime::detail
