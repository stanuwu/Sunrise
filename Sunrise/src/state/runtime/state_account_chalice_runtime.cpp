/** Chalice of Opulence: rune and upgrade plugs checked against the account's unlock banks. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../build_data/runtime.h"
#include "../rewards/reward_resolver.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail {

namespace {

namespace item_details = build_data::items::details;
namespace item_insertion_rules = build_data::items::insertion_rules;

/** One upgrade the Chalice sells: the plug that buys it and the plug that shows it owned. */
struct UpgradeOffer {
    std::uint32_t purchasePlugHash{};
    std::uint32_t ownedPlugHash{};
};

// --- Authored game data --------------------------------------------------------------------

// TODO: extract what a Chalice purchase buys and what its rune slots select; no extractor
// relates a purchase plug to the upgrade it owns, and no bank holds a kind-4 value slot.

/** The Chalice of Opulence. */
constexpr std::uint32_t kChaliceHash = 1115550924U;

/**
 * The three rune slot unlocks, then Rune Compatibility, Rune Bonus and Power and Efficiency, each
 * in tier order. An owned plug's insertion rules require the flag its purchase sets.
 */
constexpr std::array<UpgradeOffer, 12> kUpgradeOffers{{
    {1793704178U, 1456563168U},
    {3586402893U, 1456563169U},
    {1004723440U, 1456563170U},
    {2641705418U, 2757881565U},
    {1493606787U, 2757881566U},
    {2031415176U, 2757881567U},
    {2910484429U, 318003668U},
    {328804976U, 318003671U},
    {4131384203U, 318003670U},
    {1065356893U, 2458328740U},
    {3612874624U, 2458328743U},
    {4195424667U, 2458328742U},
}};

/** A rune slots only while the balance its plug's quantity counts holds at least this many. */
constexpr std::int32_t kRuneSlotBalance = 1;

/**
 * A rune lane's rules read the rune selected in the lane before from a kind-4 value slot, which
 * no bank holds. The server stands in this value for a selected rune, and zero for none.
 */
constexpr std::uint32_t kSelectedRuneValue = 1;

// The upgrade's flag and its cost's debits fit the transaction's unlock writes together.
static_assert(kSocketUnlockWriteCapacity
              >= build_data::material_requirements::kRequirementCapacity + 1);

/** @return The offer a purchase plug buys, or null for any other plug. */
[[nodiscard]] const UpgradeOffer* find_upgrade(std::uint32_t plugHash) noexcept {
    for (const UpgradeOffer& offer : kUpgradeOffers) {
        if (offer.purchasePlugHash == plugHash) {
            return &offer;
        }
    }
    return nullptr;
}

/** @return True for a rune: a Chalice plug whose quantity counts an account balance. */
[[nodiscard]] bool is_rune(const build_data::items::Definition& plug) noexcept {
    return plug.quantityValueIndex != build_data::items::kUnavailableValueIndex;
}

/** @return True when a lane holds a rune. */
[[nodiscard]] bool holds_rune(const account::inventory::Sockets& sockets,
                              std::uint8_t lane) noexcept {
    build_data::items::Definition plug{};
    return lane < sockets.plugs.size() && sockets.plugs[lane].has_value()
           && build_data::find_item_definition_hash(*sockets.plugs[lane], plug) && is_rune(plug);
}

/**
 * Replaces a rune lane's unbound value reads with the rune selected in the lane before.
 * @return False for an unbound flag, or an unbound value read outside a rune lane.
 */
[[nodiscard]] bool bind_selected_rune(std::span<build_data::rewards::Instruction> program,
                                      bool runeLane,
                                      const account::inventory::Sockets& sockets,
                                      std::uint8_t lane) noexcept {
    for (build_data::rewards::Instruction& instruction : program) {
        if (instruction.bank != unlocks::Bank::external) {
            continue;
        }
        if (!runeLane || lane == 0 || instruction.opcode != unlocks::Opcode::loadValue) {
            return false;
        }
        const bool selected = holds_rune(sockets, static_cast<std::uint8_t>(lane - 1));
        instruction = {unlocks::Opcode::constant,
                       unlocks::Bank::none,
                       selected ? kSelectedRuneValue : 0U};
    }
    return true;
}

/**
 * Evaluates a plug's insertion rules over the saved banks; a plug that declares none passes.
 * @return False when the rules do not read or do not hold.
 */
[[nodiscard]] bool rules_hold(const build_data::items::Definition& plug,
                              const unlocks::Table& banks,
                              CharacterClass characterClass,
                              const account::inventory::Sockets& sockets,
                              std::uint8_t lane) noexcept {
    item_insertion_rules::Rule rule{};
    if (!build_data::find_plug_insertion_rule(plug.definitionIndex, rule)) {
        return true;
    }
    const auto program = std::span(rule.condition).first(rule.conditionCount);
    bool held = false;
    return rule.readable && bind_selected_rune(program, is_rune(plug), sockets, lane)
           && reward_resolution::eligible(program, {banks, characterClass, 0, nullptr}, held)
           && held;
}

/** @return True when the plug sets offering a plug require no flag, or theirs is set. */
[[nodiscard]] bool offered(const build_data::items::Definition& plug,
                           const unlocks::Table& banks) noexcept {
    return plug.plugSetFlagIndex == build_data::items::kUnavailableFlagIndex
           || banks.accountFlags[plug.plugSetFlagIndex] == unlocks::kFlagSet;
}

/**
 * Sets the flag of the upgrade a purchase buys and picks the plug its lane shows.
 * @param banks Saved banks; receive the flag, so the bought upgrade's rules see it.
 * @param reason Receives a static label when the result is false.
 * @return False when the upgrade is owned, its flag does not resolve or its rules then fail.
 */
[[nodiscard]] bool buy_upgrade(const UpgradeOffer& offer,
                               const item_details::Definition& chalice,
                               const account::inventory::Sockets& sockets,
                               std::uint8_t lane,
                               unlocks::Table& banks,
                               CharacterClass characterClass,
                               ChaliceUpgrade& upgrade,
                               const char*& reason) noexcept {
    build_data::items::Definition owned{};
    item_insertion_rules::Rule ownedRule{};
    if (!build_data::find_item_definition_hash(offer.ownedPlugHash, owned)) {
        reason = "chalice_upgrade_plug";
        return false;
    }
    if (!build_data::find_plug_insertion_rule(owned.definitionIndex, ownedRule)
        || ownedRule.requiredFlagIndex == item_insertion_rules::kUnavailableFlagIndex) {
        reason = "chalice_upgrade_flag";
        return false;
    }
    if (banks.accountFlags[ownedRule.requiredFlagIndex] == unlocks::kFlagSet) {
        reason = "chalice_upgrade_owned";
        return false;
    }
    if (!write_account_unlock(upgrade.unlockWrites,
                              AccountUnlockBank::flags,
                              ownedRule.requiredFlagIndex,
                              unlocks::kFlagSet)) {
        reason = "chalice_upgrade_write";
        return false;
    }
    banks.accountFlags[ownedRule.requiredFlagIndex] = unlocks::kFlagSet;
    if (!rules_hold(owned, banks, characterClass, sockets, lane)) {
        reason = "chalice_upgrade_rules";
        return false;
    }
    if (build_data::is_socket_plug_allowed(chalice.definitionIndex, lane, owned.definitionIndex)) {
        upgrade.lanePlug = owned;
        return true;
    }
    const std::uint16_t initialPlugIndex = chalice.initialPlugIndices[lane];
    if (initialPlugIndex == item_details::kUnavailableItemIndex
        || !build_data::find_item_definition_index(initialPlugIndex, upgrade.lanePlug)) {
        reason = "chalice_initial_plug";
        return false;
    }
    return true;
}

} // namespace

/** Checks a Chalice plug's offer, rules and rune balance, and stages an upgrade purchase. */
ChaliceAction stage_chalice_plug(const AccountState& account,
                                 std::size_t characterIndex,
                                 const item_details::Definition& chalice,
                                 const account::inventory::Sockets& sockets,
                                 std::uint8_t socketLane,
                                 const build_data::items::Definition& requested,
                                 ChaliceUpgrade& upgrade,
                                 const char*& reason) noexcept {
    upgrade = {};
    reason = nullptr;
    if (chalice.definitionHash != kChaliceHash) {
        return ChaliceAction::none;
    }
    const CharacterClass characterClass = account.characters[characterIndex].characterClass;
    unlocks::Table banks{};
    if (!build_data::plug_insertion_rules_ready()) {
        reason = "chalice_rules_not_ready";
        return ChaliceAction::refused;
    }
    if (!investment::store::read_unlocks(banks, static_cast<int>(characterIndex))) {
        reason = "chalice_unlock_read";
        return ChaliceAction::refused;
    }
    if (!offered(requested, banks)) {
        reason = "chalice_not_offered";
        return ChaliceAction::refused;
    }
    if (!rules_hold(requested, banks, characterClass, sockets, socketLane)) {
        reason = "chalice_plug_rules";
        return ChaliceAction::refused;
    }
    if (is_rune(requested)) {
        // A rune lane is open while the rules of its empty plug, which its slot unlock sets, hold.
        const std::uint16_t emptyPlugIndex = chalice.initialPlugIndices[socketLane];
        build_data::items::Definition emptyPlug{};
        if (emptyPlugIndex == item_details::kUnavailableItemIndex
            || !build_data::find_item_definition_index(emptyPlugIndex, emptyPlug)
            || !rules_hold(emptyPlug, banks, characterClass, sockets, socketLane)) {
            reason = "chalice_rune_lane";
            return ChaliceAction::refused;
        }
        if (banks.objectiveValues[requested.quantityValueIndex] < kRuneSlotBalance) {
            reason = "chalice_rune_balance";
            return ChaliceAction::refused;
        }
        return ChaliceAction::none;
    }
    const UpgradeOffer* offer = find_upgrade(requested.definitionHash);
    if (offer == nullptr) {
        return ChaliceAction::none;
    }
    return buy_upgrade(
               *offer, chalice, sockets, socketLane, banks, characterClass, upgrade, reason)
               ? ChaliceAction::upgraded
               : ChaliceAction::refused;
}

} // namespace sunrise::state::runtime::detail
