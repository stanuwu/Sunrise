/** Gambit Prime Synthesizer exchanges: a synthesis plug pays a Mote, a recycle plug its Synths. */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../account/inventory/synthesizer_tier.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail {

namespace {

namespace item_details = build_data::items::details;

/** One Mote and the two Synthesizer plugs that exchange it. */
struct MoteOutput {
    std::uint32_t synthesisPlugHash{};
    std::uint32_t recyclePlugHash{};
    std::uint32_t moteHash{};
};

// --- Authored game data --------------------------------------------------------------------

// TODO: extract what each Synthesizer plug pays out; no extractor relates a synthesis plug to its
// Mote, a recycle plug to its refund, or either plug to its lane's reset.

/**
 * Four roles, each in weak, middling and powerful order. A synthesis plug's own cost names its
 * Synths and a recycle plug's own cost names its Mote; neither names what the exchange pays.
 */
constexpr std::array<MoteOutput, 12> kSynthesizerMoteOutputs{{
    {456408693U, 1431423016U, 3611551832U},
    {456408692U, 1431423017U, 3611551833U},
    {456408695U, 1431423018U, 3611551834U},
    {3728290549U, 1401078730U, 2657400314U},
    {3728290548U, 1401078731U, 2657400315U},
    {3728290551U, 1401078728U, 2657400312U},
    {3602650887U, 882903534U, 3162804766U},
    {3602650886U, 882903535U, 3162804767U},
    {3602650885U, 882903532U, 3162804764U},
    {1769115309U, 248465794U, 753747954U},
    {1769115308U, 248465795U, 753747955U},
    {1769115311U, 248465792U, 753747952U},
}};

/** @return The output whose synthesis or recycle plug this is, or null for any other plug. */
[[nodiscard]] const MoteOutput* find_mote_output(std::uint32_t plugHash) noexcept {
    for (const MoteOutput& output : kSynthesizerMoteOutputs) {
        if (output.synthesisPlugHash == plugHash || output.recyclePlugHash == plugHash) {
            return &output;
        }
    }
    return nullptr;
}

/** @return True when the profile holds a stack of this item. */
[[nodiscard]] bool holds_profile_item(const AccountState& account,
                                      std::uint32_t definitionHash) noexcept {
    for (const auto& item : std::span(account.profileItems).first(account.profileItemCount)) {
        if (item.definitionHash == definitionHash) {
            return true;
        }
    }
    return false;
}

/**
 * Reads the one unconditional, consumed requirement a Synthesizer plug's insertion cost holds.
 * @return False when the plug is free or its cost is anything but one such row.
 */
[[nodiscard]] bool single_cost(std::uint32_t plugHash,
                               build_data::items::Definition& plug,
                               build_data::material_requirements::Definition& cost) noexcept {
    return build_data::find_item_definition_hash(plugHash, plug)
           && plug.insertionMaterialRequirementSetIndex
                  != build_data::items::kUnavailableMaterialRequirementSetIndex
           && build_data::find_material_requirement_set(plug.insertionMaterialRequirementSetIndex,
                                                        cost)
           && cost.requirementCount == 1 && cost.requirements[0].quantity > 0
           && cost.requirements[0].deleteOnAction
           && cost.requirements[0].condition
                  == build_data::material_requirements::kUnconditionalRequirement;
}

/**
 * The Synthesizer keeps one Mote out per tier, whichever role it is, so a synthesis lane refuses
 * while the profile holds a Mote that any plug of the lane's pool synthesizes.
 * @param reason Receives a static label when the result is false.
 * @return False when the lane's Mote is out or an authored plug does not resolve.
 */
[[nodiscard]] bool lane_mote_absent(const AccountState& account,
                                    std::uint16_t synthesizerIndex,
                                    std::uint8_t lane,
                                    const char*& reason) noexcept {
    for (const MoteOutput& output : kSynthesizerMoteOutputs) {
        build_data::items::Definition plug{};
        if (!build_data::find_item_definition_hash(output.synthesisPlugHash, plug)) {
            reason = "synthesizer_plug_definition";
            return false;
        }
        if (holds_profile_item(account, output.moteHash)
            && build_data::is_socket_plug_allowed(synthesizerIndex, lane, plug.definitionIndex)) {
            reason = "synthesizer_mote_held";
            return false;
        }
    }
    return true;
}

/**
 * Stages the Mote a synthesis plug pays while its tier holds none.
 * @param reason Receives a static label when the result is false.
 */
[[nodiscard]] bool stage_synthesis(const AccountState& account,
                                   const item_details::Definition& synthesizer,
                                   std::uint8_t lane,
                                   const MoteOutput& output,
                                   SynthesizerPayout& payout,
                                   const char*& reason) noexcept {
    build_data::items::Definition plug{};
    build_data::items::Definition mote{};
    build_data::material_requirements::Definition cost{};
    if (!single_cost(output.synthesisPlugHash, plug, cost)) {
        reason = "synthesizer_synthesis_cost";
        return false;
    }
    if (!build_data::find_item_definition_hash(output.moteHash, mote)) {
        reason = "synthesizer_mote_definition";
        return false;
    }
    if (!lane_mote_absent(account, synthesizer.definitionIndex, lane, reason)) {
        return false;
    }
    payout.itemIndex = mote.definitionIndex;
    payout.quantity = 1;
    return true;
}

/**
 * Stages a refund of what the Mote's synthesis plug costs on this Synthesizer, socket scalars
 * included, so recycling reverses synthesis exactly. The recycle plug's own cost debits the Mote.
 * @param reason Receives a static label when the result is false.
 */
[[nodiscard]] bool stage_recycle(const item_details::Definition& synthesizer,
                                 const MoteOutput& output,
                                 SynthesizerPayout& payout,
                                 const char*& reason) noexcept {
    build_data::items::Definition recycle{};
    build_data::items::Definition synthesis{};
    build_data::items::Definition mote{};
    build_data::material_requirements::Definition recycleCost{};
    build_data::material_requirements::Definition synthesisCost{};
    if (!single_cost(output.recyclePlugHash, recycle, recycleCost)) {
        reason = "synthesizer_recycle_cost";
        return false;
    }
    if (!single_cost(output.synthesisPlugHash, synthesis, synthesisCost)) {
        reason = "synthesizer_synthesis_cost";
        return false;
    }
    if (!build_data::find_item_definition_hash(output.moteHash, mote)) {
        reason = "synthesizer_mote_definition";
        return false;
    }
    if (recycleCost.requirements[0].itemDefinitionIndex != mote.definitionIndex) {
        reason = "synthesizer_recycle_item";
        return false;
    }
    std::uint8_t lane = 0;
    while (lane < synthesizer.ordinarySocketCount
           && !build_data::is_socket_plug_allowed(
               synthesizer.definitionIndex, lane, synthesis.definitionIndex)) {
        ++lane;
    }
    if (lane == synthesizer.ordinarySocketCount) {
        reason = "synthesizer_synthesis_lane";
        return false;
    }
    if (!scale_socket_costs(synthesizer.socketTypes[lane], synthesisCost)) {
        reason = "synthesizer_refund_scale";
        return false;
    }
    const std::uint32_t refund = synthesisCost.requirements[0].quantity;
    if (refund > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        reason = "synthesizer_refund_overflow";
        return false;
    }
    payout.itemIndex = synthesisCost.requirements[0].itemDefinitionIndex;
    payout.quantity = static_cast<std::int32_t>(refund);
    return true;
}

} // namespace

/** @return True when the item is one of the Synthesizer's Motes. */
bool synthesizer_mote(std::uint32_t definitionHash) noexcept {
    return std::any_of(kSynthesizerMoteOutputs.begin(),
                       kSynthesizerMoteOutputs.end(),
                       [definitionHash](const MoteOutput& output) {
                           return output.moteHash == definitionHash;
                       });
}

/** Stages what a Synthesizer exchange pays and the initial plug its lane returns to. */
SynthesizerExchange stage_synthesizer_payout(const AccountState& account,
                                             const item_details::Definition& synthesizer,
                                             std::uint8_t socketLane,
                                             const build_data::items::Definition& requested,
                                             SynthesizerPayout& payout,
                                             const char*& reason) noexcept {
    payout = {};
    reason = nullptr;
    const MoteOutput* output = find_mote_output(requested.definitionHash);
    if (output == nullptr) {
        return SynthesizerExchange::none;
    }
    const bool staged =
        output->recyclePlugHash == requested.definitionHash
            ? stage_recycle(synthesizer, *output, payout, reason)
            : stage_synthesis(account, synthesizer, socketLane, *output, payout, reason);
    if (!staged) {
        return SynthesizerExchange::refused;
    }
    const std::uint16_t initialPlugIndex = synthesizer.initialPlugIndices[socketLane];
    if (initialPlugIndex == item_details::kUnavailableItemIndex
        || !build_data::find_item_definition_index(initialPlugIndex, payout.lanePlug)) {
        reason = "synthesizer_initial_plug";
        return SynthesizerExchange::refused;
    }
    return SynthesizerExchange::paid;
}

} // namespace sunrise::state::runtime::detail

/** Reads each character's tier from the one Synthesizer it holds, and the row the tiers fill. */
void sunrise::state::synthesizer_tier_values(
    const AccountState& account,
    std::uint16_t& valueRow,
    std::array<std::int32_t, kCharacterCapacity>& tiers) noexcept {
    build_data::constants::InvestmentConstants constants{};
    valueRow = build_data::find_investment_constants(constants)
                   ? constants.synthesizerTierValueRow
                   : build_data::constants::kUnboundValueRow;
    tiers.fill(account::inventory::kNoSynthesizerTier);
    const auto characters =
        std::span(account.characters).first((std::min)(account.characterCount, tiers.size()));
    for (std::size_t index = 0; index < characters.size(); ++index) {
        const auto& items = characters[index].inventory;
        const std::size_t held = account::inventory::held_synthesizer(items);
        if (held != items.count) {
            tiers[index] = account::inventory::synthesizer_tier(items.values[held].definitionHash);
        }
    }
}
