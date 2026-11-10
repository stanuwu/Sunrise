#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store.h"
#include "runtime.h"

namespace sunrise::state {
template <typename Account> struct SavedSelection;
}

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
/**
 * Spends one vendor sale row's price through the same engine Collections materials use.
 * @param price The row's cost entries; empty charges nothing.
 * @return True when every entry was paid, or the price was empty.
 */
[[nodiscard]] bool apply_sale_price(const AccountState& before,
                                    std::span<const build_data::vendors::SaleCost> price,
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
};

/** @return The selected character's index, or the character count when none is selected. */
[[nodiscard]] std::size_t selected_character_index(const AccountState& account) noexcept;
/**
 * @param quest A valid, nonempty first-step plan.
 * @return The saved value bank of the plan's scope.
 */
[[nodiscard]] investment::store::Bank
quest_bank(const build_data::items::QuestInitialization& quest) noexcept;
/**
 * @param table One unlock snapshot of the account and the selected character.
 * @param quest A nonempty plan whose row fits its bank.
 * @return The saved value the plan's row holds in that snapshot.
 */
[[nodiscard]] std::int32_t&
quest_value(unlocks::Table& table, const build_data::items::QuestInitialization& quest) noexcept;
/**
 * @param table One unlock snapshot of the account and the selected character.
 * @param credit A credit scoped to the account or the character, whose index fits its bank.
 * @return The progress lane the credited progression holds in that snapshot.
 */
[[nodiscard]] std::int32_t& progression_value(unlocks::Table& table,
                                              const PreparedProgressionCredit& credit) noexcept;
/**
 * Applies to a bank image what grant_seasonal_experience will save for the same amount: the
 * seasonal lanes, and the earned-perk flags when the grant reaches a new rank. Nothing is saved.
 * @param expected XP total the grant was prepared against.
 * @param amount Positive XP the grant adds.
 * @param after The selected character's banks, updated in place.
 * @return False when the total moved since the preparation, the new total would overflow, or the
 *         account could not be read.
 */
[[nodiscard]] bool preview_seasonal_experience(std::int32_t expected,
                                               std::int32_t amount,
                                               unlocks::Table& after) noexcept;
/** What one reward grant places, removes and spends. */
struct RewardGrantRequest {
    std::span<const DirectRecordReward> rewards{};
    /** Record already claimed in the banks, or kUnclaimedRecordIndex. */
    std::uint16_t claimedRecordIndex{kUnclaimedRecordIndex};
    /** Flags of the wrappers a draw consumed, or null for direct rows. */
    const std::bitset<unlocks::kAccountFlagCapacity>* wrapperFlags{};
    /** Held pursuit or opened wrapper removed before the rows are placed, or zero. */
    std::uint64_t redeemedInstanceSoid{};
    /** Held item a row replaces, removed with the redeemed one, or zero. */
    std::uint64_t replacedInstanceSoid{};
    /** A vendor row's cost entries, spent before the rows are placed; empty for none. */
    std::span<const build_data::vendors::SaleCost> price{};
    /** Server time in Unix seconds; a granted bounty's lifetime runs from it. */
    std::int64_t nowSeconds{};
    /**
     * The caller adds a saved value, a progression credit or seasonal XP to the prepared grant,
     * which is then a grant even when it places, removes and flags nothing.
     */
    bool carriesSavedEffects{};
};
/**
 * Prepares inventory rows and consumed-wrapper flags against one account view.
 * @param refusal Receives a static reason when the result is not prepared.
 * @return Deferred only when State may still change the answer; unresolvable is final.
 */
[[nodiscard]] RewardPreparation prepare_reward_grant(const RewardGrantRequest& request,
                                                     PendingRecordRewardGrant& mutation,
                                                     const char** refusal) noexcept;
/**
 * Replaces the socket lanes a reward names; other lanes keep their initial plugs.
 * @param sockets Receives the authored socket block; unchanged on failure.
 * @param refusal Receives a static reason on failure when not null.
 */
[[nodiscard]] bool
apply_reward_sockets(const build_data::items::details::Definition& detail,
                     std::span<const build_data::rewards::SocketOverride> overrides,
                     account::inventory::Sockets& sockets,
                     const char** refusal = nullptr) noexcept;
/**
 * A bounty starts with its deadline, however it is acquired. Other items, and a bounty whose
 * contract is not read, start with no per-instance values.
 * @param nowSeconds Server time in Unix seconds; the bounty's lifetime runs from it.
 * @return False when the deadline does not fit its signed lane.
 */
[[nodiscard]] bool
acquired_objectives(std::uint16_t definitionIndex,
                    std::int64_t nowSeconds,
                    std::optional<account::inventory::ItemObjectives>& objectives) noexcept;
/**
 * Hold investment::store::g_mutex while capturing inventory and quest state together.
 * @param account State before any acquisition charge.
 * @param chargedAccount State after the prepared material charge.
 * @param definitionHash Item definition to grant.
 * @param profileChanged Whether the charge changed profile inventory.
 * @param source Grant identity and material requirements for commit checks.
 * @param objectives Per-instance values the new item starts with, if any.
 * @param mutation Receives a pending grant; use only on success.
 * @return False when the item, inventory, mapping, or saved quest state is invalid.
 */
[[nodiscard]] bool
finalize_item_acquisition(const AccountState& account,
                          const AccountState& chargedAccount,
                          std::uint32_t definitionHash,
                          bool profileChanged,
                          const GrantSource& source,
                          const std::optional<account::inventory::ItemObjectives>& objectives,
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
 * @param nowSeconds Server time in Unix seconds a bake's bounty deadlines are compared with; a
 *        re-staging passes the time the first staging recorded.
 * @param pinnedPlugHash For a rolled socket's apply or re-roll, the result plug an earlier
 *        staging rolled, so a re-staging reproduces the same after-image; 0 rolls afresh.
 */
[[nodiscard]] bool stage_socket_plug(const AccountState& snapshot,
                                     std::size_t characterIndex,
                                     std::uint64_t targetInstanceSoid,
                                     std::uint8_t socketLane,
                                     std::uint16_t plugDefinitionIndex,
                                     std::int64_t nowSeconds,
                                     PendingSocketPlug& mutation,
                                     std::uint32_t pinnedPlugHash = 0) noexcept;
/**
 * Multiplies each requirement by the target socket type's authored scalar for its currency.
 * @return False before the scalars are published or when a scaled quantity overflows.
 */
[[nodiscard]] bool
scale_socket_costs(std::uint16_t socketType,
                   build_data::material_requirements::Definition& materialSet) noexcept;

/**
 * Pays one profile stack the way a grant lands, but only in full and into free rows: an exchange
 * is refused rather than paid in part or at the cost of an evicted row. Paid after the action's
 * charge, so the charge's compaction cannot move a credited row off the serial the ring names.
 * @param uncharged Account the action began with; credited serials rise above all of its rows.
 * @param account Charged account; receives the credit.
 * @param changes Receives the rows the credit created or grew, for the account's change ring.
 * @param changeCount Rows already in changes; receives the new count.
 */
[[nodiscard]] bool
credit_profile_stack(const AccountState& uncharged,
                     AccountState& account,
                     std::size_t characterIndex,
                     std::uint16_t itemDefinitionIndex,
                     std::int32_t quantity,
                     std::array<ProfileStackChange, kProfileStackChangeCapacity>& changes,
                     std::size_t& changeCount) noexcept;

/** What a Synthesizer plug pays once its insertion cost is charged, and its lane's next plug. */
struct SynthesizerPayout {
    std::uint16_t itemIndex{};
    std::int32_t quantity{};
    /** Rows the payout created or grew. */
    std::array<ProfileStackChange, kProfileStackChangeCapacity> changes{};
    std::size_t changeCount{};
    build_data::items::Definition lanePlug{};
};

/** Whether a requested plug is a Synthesizer exchange, and whether that exchange is staged. */
enum class SynthesizerExchange : std::uint8_t { none, paid, refused };

/**
 * Decides what a Gambit Prime Synthesizer exchange pays before the plug's insertion cost is
 * charged. A synthesis plug pays its Mote and a recycle plug refunds the Synths that Mote cost;
 * either lane returns to its initial plug.
 * @param account Account before the insertion cost.
 * @param synthesizer Detail of the item the plug was requested on, whose lane accepts the plug.
 * @param reason Receives a static label when the exchange is refused, else null.
 * @return None for any other plug, which leaves the payout empty.
 */
[[nodiscard]] SynthesizerExchange
stage_synthesizer_payout(const AccountState& account,
                         const build_data::items::details::Definition& synthesizer,
                         std::uint8_t socketLane,
                         const build_data::items::Definition& requested,
                         SynthesizerPayout& payout,
                         const char*& reason) noexcept;

/** @return True when the item is one of the Synthesizer's Motes. */
[[nodiscard]] bool synthesizer_mote(std::uint32_t definitionHash) noexcept;

/** What a Dawning oven plug bakes: its cost, its cookie, its discovery and the reset lanes. */
struct OvenBake {
    /** The recipe's cost for a Combine that matched one, otherwise the requested plug's own. */
    std::uint16_t costSetIndex{build_data::items::kUnavailableMaterialRequirementSetIndex};
    /** Ingredient balances a burnt bake debits ahead of its cost. */
    AccountUnlockWrites unlockWrites{};
    std::uint16_t cookieIndex{};
    /** Flag of a recipe a Combine matched, or unavailable for a burnt or recipe-lane bake. */
    std::uint16_t discoveryFlagIndex{build_data::items::kUnavailableFlagIndex};
    /** The oven's lanes after the bake, and the plug the requested lane returns to. */
    account::inventory::Sockets sockets{};
    build_data::items::Definition lanePlug{};
    /** Cookie rows the payout created or grew. */
    std::array<ProfileStackChange, kProfileStackChangeCapacity> changes{};
    std::size_t changeCount{};
};

/** Whether a requested plug is an oven action, and whether it bakes or is refused. */
enum class OvenAction : std::uint8_t {
    /** Any other plug, or a Masterwork the oven's discoveries allow, which lands as selected. */
    none,
    baked,
    refused,
};

/**
 * Decides what a Dawning oven plug bakes before its cost is charged. A Combine bakes the recipe
 * its two ingredients pair or a burnt cookie; a recipe plug bakes its discovered recipe, the
 * discounted variant only on a masterworked oven. The Masterwork plug needs every recipe
 * discovered. A bake returns the ingredient, Combine and recipe lanes to their initial plugs.
 * @param oven Detail of the item the plug was requested on, whose lane accepts the plug.
 * @param sockets The oven's current lanes, native defaults materialized.
 * @param reason Receives a static label when the plug is refused, else null.
 * @return None for any other plug, which leaves the bake empty.
 */
[[nodiscard]] OvenAction stage_oven_bake(const build_data::items::details::Definition& oven,
                                         const account::inventory::Sockets& sockets,
                                         std::uint8_t socketLane,
                                         const build_data::items::Definition& requested,
                                         OvenBake& bake,
                                         const char*& reason) noexcept;

/**
 * Pays a staged bake once its cost is charged: credits the cookie, so its rows keep the serials
 * the change ring names, sets the discovery flag of a newly baked recipe and advances the Dawning
 * bounties a recipe bake counts.
 * @param uncharged Account the bake began with.
 * @param account Charged account; receives the cookie and the advanced bounties.
 * @param writes The transaction's unlock writes; receive the discovery flag.
 * @param nowSeconds Server time in Unix seconds, which a bounty's deadline is compared with.
 */
[[nodiscard]] bool pay_oven_bake(const AccountState& uncharged,
                                 AccountState& account,
                                 std::size_t characterIndex,
                                 OvenBake& bake,
                                 AccountUnlockWrites& writes,
                                 std::int64_t nowSeconds) noexcept;

/**
 * Prepares crediting one Dawning ingredient pickup to the oven balance its ingredient plug counts.
 * The caller previews the write over its unlock after-image and writes it in its own transaction,
 * after checking that the row still holds the observed value.
 * @param pickupHash Ingredient pickup item the Client acquired.
 * @param quantity Positive units to credit.
 * @param credit Receives the account value row, the value it holds and the credited value.
 * @return Unresolvable for any other item, a pickup whose plug is not installed or a plug with no
 *         balance row; not ready when the balance cannot be read; deferred when it cannot hold
 *         the credit.
 */
[[nodiscard]] RewardPreparation prepare_ingredient_credit(std::uint32_t pickupHash,
                                                          std::int32_t quantity,
                                                          AccountUnlockWrite& credit) noexcept;

/**
 * @return True for a Dawning ingredient pickup, which arrives only with the oven balance credit
 *         prepare_ingredient_credit stages.
 */
[[nodiscard]] bool is_ingredient_pickup(std::uint32_t definitionHash) noexcept;

/**
 * Prepares opening a newly acquired oven's ingredient chooser. The chooser reads the Dawning Begins
 * tutorial's quest row, which a tutorial never started and with no item held is set to ready.
 * The caller previews and writes it as prepare_ingredient_credit's caller does.
 * @param account Account the oven lands in.
 * @param readiness Receives the quest row write, or stays empty when the row keeps its value.
 * @return Unresolvable when the published contracts name no one account row for the tutorial;
 *         not ready while the contracts are unpublished or the row cannot be read.
 */
[[nodiscard]] RewardPreparation
prepare_chooser_readiness(const AccountState& account,
                          std::optional<AccountUnlockWrite>& readiness) noexcept;

/** @return True for the Dawning oven, whose acquisition stages prepare_chooser_readiness. */
[[nodiscard]] bool is_dawning_oven(std::uint32_t definitionHash) noexcept;

/** What advances a Dawning bounty objective. */
enum class DawningEvent : std::uint8_t {
    /** A recipe's cookie came out of the oven. */
    bake,
    /** A vendor row's price spent a recipe's cookie. */
    delivery,
};

/**
 * Advances by one each held Dawning bounty objective the event counts, and gives each advanced
 * bounty a new mutation serial so the client re-reads its progress. An expired bounty or a
 * finished objective is left alone, and a delivery counts only a recipe's cookie.
 * @param cookieHash The cookie baked or spent.
 * @param nowSeconds Server time in Unix seconds, which a bounty's deadline is compared with.
 * @return Unresolvable when a held bounty's published contract does not hold the objective the
 *         rules name; not ready while the contracts are unpublished; deferred when the bounty's
 *         saved lane is negative or the character's serials have run out.
 */
[[nodiscard]] RewardPreparation credit_dawning_bounties(CharacterState& character,
                                                        DawningEvent event,
                                                        std::uint32_t cookieHash,
                                                        std::int64_t nowSeconds) noexcept;

/**
 * Finds the lane of a held pursuit's own values that one item value slot names.
 * @param slot Item value slot an objective reads.
 * @param lane Receives the slot's position in the contract's value list.
 * @return False when the contract does not carry the slot, the lane lies past an item's values, or
 *         it is a bounty's deadline lane, which holds no progress.
 */
[[nodiscard]] bool item_value_lane(const build_data::pursuits::Definition& contract,
                                   std::uint16_t slot,
                                   std::size_t& lane) noexcept;

/**
 * @param itemValues The held pursuit's own values; a bounty keeps its deadline in the expiry lane.
 * @param nowSeconds Server time in Unix seconds.
 * @return True for a bounty whose deadline is unset or has passed.
 */
[[nodiscard]] bool bounty_expired(const build_data::pursuits::Definition& contract,
                                  const account::inventory::ItemObjectives& itemValues,
                                  std::int64_t nowSeconds) noexcept;

/**
 * Finds the item lane one objective's progress lives in, and its target.
 * @param ordinal Objective position in the contract.
 * @return False when the objective's progress is not one of the item's own values, or its target
 *         is not a plain count up to its completion value.
 */
[[nodiscard]] bool objective_lane(const build_data::pursuits::Definition& contract,
                                  std::uint8_t ordinal,
                                  std::size_t& lane,
                                  std::int32_t& target) noexcept;

/** @return True when every catalog a pursuit redemption reads is published. */
[[nodiscard]] bool pursuit_redemption_catalogs_ready() noexcept;

/**
 * Judges one held pursuit and plans its redemption as prepare_pursuit_redemption does, against a
 * saved view the caller read under the store lock it still holds. The view is only read, so one
 * read serves every pursuit a sweep judges.
 * @param saved The selected character's saved view.
 * @param mutation Receives the prepared grant; emptied first.
 * @param reason Receives a static reason when the result is not prepared.
 */
[[nodiscard]] PursuitRedemption plan_pursuit_redemption(SavedSelection<AccountState>& saved,
                                                        std::uint64_t instanceSoid,
                                                        std::uint32_t definitionHash,
                                                        std::int64_t nowSeconds,
                                                        std::uint64_t seed,
                                                        PendingRecordRewardGrant& mutation,
                                                        const char*& reason) noexcept;

/** The flag a Chalice upgrade purchase sets, and the plug its lane shows after it. */
struct ChaliceUpgrade {
    AccountUnlockWrites unlockWrites{};
    build_data::items::Definition lanePlug{};
};

/** Whether a requested plug is on the Chalice, and whether it buys an upgrade or is refused. */
enum class ChaliceAction : std::uint8_t {
    /** Any other item, or a Chalice plug whose checks hold, which lands as selected. */
    none,
    upgraded,
    refused,
};

/**
 * Checks a Chalice of Opulence plug before its cost is charged: the plug sets offering it and its
 * insertion rules must hold over the saved banks, and a rune needs one in its balance. An upgrade
 * purchase sets its upgrade's flag; its lane shows the bought upgrade when the lane offers it and
 * otherwise returns to its initial plug.
 * @param account Account the action began with.
 * @param chalice Detail of the item the plug was requested on, whose lane accepts the plug.
 * @param sockets The item's current lanes, native defaults materialized.
 * @param reason Receives a static label when the plug is refused, else null.
 * @return None for any other item, which leaves the upgrade empty.
 */
[[nodiscard]] ChaliceAction
stage_chalice_plug(const AccountState& account,
                   std::size_t characterIndex,
                   const build_data::items::details::Definition& chalice,
                   const account::inventory::Sockets& sockets,
                   std::uint8_t socketLane,
                   const build_data::items::Definition& requested,
                   ChaliceUpgrade& upgrade,
                   const char*& reason) noexcept;
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
                                           std::uint64_t& output,
                                           std::uint64_t minimum = 0) noexcept;
[[nodiscard]] bool next_profile_item_instance_soid(const AccountState& account,
                                                   std::uint64_t& output) noexcept;
[[nodiscard]] bool account_owns_soid(const AccountState& account, std::uint64_t soid) noexcept;
[[nodiscard]] std::int32_t acquisition_level(const CharacterState& character) noexcept;
[[nodiscard]] bool stage_item_dismantle(const AccountState& account,
                                        std::size_t characterIndex,
                                        std::uint64_t instanceSoid,
                                        bool paysRewards,
                                        PendingItemDismantle& mutation) noexcept;
[[nodiscard]] bool stage_stack_discard(const AccountState& account,
                                       std::size_t characterIndex,
                                       std::uint32_t definitionHash,
                                       std::int32_t heldQuantity,
                                       PendingItemDismantle& mutation) noexcept;
[[nodiscard]] bool stage_profile_resident_discard(const AccountState& account,
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
/** @return The saved bank that holds one account unlock bank's rows. */
[[nodiscard]] constexpr investment::store::Bank store_bank(AccountUnlockBank bank) noexcept {
    return bank == AccountUnlockBank::flags ? investment::store::Bank::accountFlags
                                            : investment::store::Bank::objectiveValues;
}
/**
 * Reads one account unlock row as a transaction's earlier writes leave it.
 * @return False when the saved row cannot be read.
 */
[[nodiscard]] bool read_account_unlock(const AccountUnlockWrites& writes,
                                       AccountUnlockBank bank,
                                       std::uint16_t index,
                                       std::int32_t& value) noexcept;
/**
 * Stages one account unlock row's new value over any earlier write to it.
 * @return False when the row cannot be read or the writes are full.
 */
[[nodiscard]] bool write_account_unlock(AccountUnlockWrites& writes,
                                        AccountUnlockBank bank,
                                        std::uint16_t index,
                                        std::int32_t value) noexcept;
/** @return False when the account value row holds less than quantity or cannot be staged. */
[[nodiscard]] bool debit_account_value(AccountUnlockWrites& writes,
                                       std::uint16_t index,
                                       std::uint32_t quantity) noexcept;
/**
 * Applies one dense installed action-cost set resolved from the selected plug or action row. A
 * value-slot row counts and debits its account value row instead of inventory, as the native cost
 * reader does; one whose slot maps to no account row is refused.
 * @param writes Receives the value-slot debits over the action's earlier writes; unchanged on
 *        failure.
 */
[[nodiscard]] bool
apply_action_materials(const AccountState& before,
                       const build_data::material_requirements::Definition& definition,
                       AccountState& after,
                       bool& changed,
                       AccountUnlockWrites& writes) noexcept;
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
