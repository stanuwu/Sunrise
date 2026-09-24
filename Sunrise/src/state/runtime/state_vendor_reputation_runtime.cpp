#include "state_vendor_reputation_runtime.h"

#include <algorithm>
#include <limits>

#include "../build_data/progressions/progression_catalog.h"
#include "../build_data/rewards/reward_catalog.h"
#include "../build_data/vendors/reconstructed_rank_claim_links.h"
#include "../build_data/vendors/reputation_sale_catalog.h"
#include "../build_data/vendors/vendor_catalog.h"
#include "../build_data/vendors/vendor_gate_catalog.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {

/** Native progression level walks read experience from lane zero. */
constexpr std::size_t kExperienceLane = 0;

/** Reply zero completes the supported normal reward interactions. */
constexpr std::uint16_t kAcceptRewardReply = 0;

/** Saved unlocks and Family-5 overrides visible to one native vendor gate. */
struct GateValues {
    const build_data::vendors::Gate& gate;
    const Family5State& family;
    const unlocks::Table& banks;
};

/** @return The native slot binding, or null when extraction did not resolve it. */
const build_data::vendors::GateInput* gate_input(const GateValues& values,
                                                 build_data::vendors::Opcode opcode,
                                                 std::uint16_t slot) noexcept {
    const auto inputs = std::span{values.gate.inputs}.first(values.gate.inputCount);
    const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto& input) {
        return input.opcode == opcode && input.slot == slot;
    });
    return found == inputs.end() ? nullptr : &*found;
}

/**
 * Reads a native flag slot, preferring a Family-5 override.
 * @param context Gate values shared by this evaluation.
 * @param slot Native flag slot.
 * @param logical Receives the logical byte on success.
 * @return False when the slot has no supported source.
 */
bool gate_flag(void* context, std::uint16_t slot, std::uint8_t& logical) noexcept {
    const auto& values = *static_cast<const GateValues*>(context);
    const auto* input = gate_input(values, build_data::vendors::Opcode::flag, slot);
    if (input == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < values.family.flagCount; ++index) {
        if (values.family.flags[index].slot == slot) {
            logical = values.family.flags[index].value;
            return true;
        }
    }
    using Bank = build_data::vendors::GateBank;
    switch (input->bank) {
    case Bank::accountFlag:
        if (input->row < values.banks.accountFlags.size()) {
            logical = values.banks.accountFlags[input->row];
            return true;
        }
        break;
    case Bank::profileFlag:
        if (input->row < values.banks.profileFlags.size()) {
            logical = values.banks.profileFlags[input->row];
            return true;
        }
        break;
    case Bank::characterFlag:
        if (input->row < values.banks.characterObjectFlags.size()) {
            logical = values.banks.characterObjectFlags[input->row];
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

/**
 * Reads a native value slot, preferring a Family-5 override.
 * @param context Gate values shared by this evaluation.
 * @param slot Native value slot.
 * @param value Receives the saved value on success.
 * @return False when the slot has no supported source.
 */
bool gate_value(void* context, std::uint16_t slot, std::int32_t& value) noexcept {
    const auto& values = *static_cast<const GateValues*>(context);
    const auto* input = gate_input(values, build_data::vendors::Opcode::loadValue, slot);
    if (input == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < values.family.valueCount; ++index) {
        if (values.family.values[index].slot == slot) {
            value = values.family.values[index].value;
            return true;
        }
    }
    using Bank = build_data::vendors::GateBank;
    switch (input->bank) {
    case Bank::accountValue:
        if (input->row < values.banks.objectiveValues.size()) {
            value = values.banks.objectiveValues[input->row];
            return true;
        }
        break;
    case Bank::characterValue:
        if (input->row < values.banks.characterObjectValues.size()) {
            value = values.banks.characterObjectValues[input->row];
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

/**
 * Evaluates one authored gate against supplied saved values.
 * @param gate Installed gate to evaluate.
 * @param family Family-5 overrides.
 * @param banks Saved unlock values.
 * @return True for an empty gate or a complete expression that evaluates true.
 */
bool gate_passes(const build_data::vendors::Gate& gate,
                 const Family5State& family,
                 const unlocks::Table& banks) noexcept {
    if (gate.program.count == 0) {
        return true;
    }
    GateValues values{gate, family, banks};
    const build_data::vendors::Inputs inputs{gate_flag, gate_value, &values};
    bool result = false;
    return build_data::vendors::evaluate(
               std::span{gate.program.instructions}.first(gate.program.count), inputs, result)
           && result;
}

enum class CreditGateKind { unrelated, claim, warning, malformed };

/**
 * Resolves a native VALUE[credit] > 0 prefix without treating a level warning as a claim.
 * @param interaction Installed interaction gate.
 * @param row Receives the saved character credit row.
 * @return The supported claim shape, a warning branch, an unrelated gate or malformed input.
 */
CreditGateKind credit_gate(const build_data::vendors::InteractionGate& interaction,
                           std::uint16_t& row) noexcept {
    namespace vendors = build_data::vendors;
    row = 0;
    const auto program = std::span{interaction.condition.program.instructions}.first(
        interaction.condition.program.count);
    if (program.size() < 2 || program[0].opcode != vendors::Opcode::loadValue
        || program[1].opcode != vendors::Opcode::constant || program[1].operand != 0) {
        return CreditGateKind::unrelated;
    }
    if (program.size() < 3 || program[0].operand < 0
        || program[0].operand > (std::numeric_limits<std::uint16_t>::max)()
        || program[2].opcode != vendors::Opcode::greaterThan) {
        return CreditGateKind::malformed;
    }
    const auto slot = static_cast<std::uint16_t>(program[0].operand);
    const auto inputs =
        std::span{interaction.condition.inputs}.first(interaction.condition.inputCount);
    const vendors::GateInput* found = nullptr;
    for (const auto& input : inputs) {
        if (input.opcode != vendors::Opcode::loadValue || input.slot != slot) {
            continue;
        }
        if (found != nullptr || input.bank != vendors::GateBank::characterValue) {
            return CreditGateKind::malformed;
        }
        found = &input;
    }
    if (found == nullptr || found->row == 0
        || found->row >= unlocks::kCharacterObjectValueCapacity) {
        return CreditGateKind::malformed;
    }
    row = found->row;
    const auto remainder = program.subspan(3);
    if (std::any_of(remainder.begin(), remainder.end(), [](const auto& instruction) {
            return instruction.opcode == vendors::Opcode::lessThan
                   || instruction.opcode == vendors::Opcode::lessOrEqual;
        })) {
        return CreditGateKind::warning;
    }
    return CreditGateKind::claim;
}

/**
 * Resolves the exact reconstructed interaction's installed credit row.
 * @param link Reconstructed interaction link.
 * @param interaction Receives the installed interaction gate.
 * @param row Receives the saved character credit row.
 * @return False when the exact link is missing, changed or not a claim gate.
 */
bool reconstructed_credit_gate(const build_data::vendors::ReconstructedRankClaimLink& link,
                               build_data::vendors::InteractionGate& interaction,
                               std::uint16_t& row) noexcept {
    namespace vendors = build_data::vendors;
    interaction = {};
    row = 0;
    return vendors::find_interaction_gate(link.vendorHash, link.interactionIndex, interaction)
           && interaction.categoryIndex == link.categoryIndex
           && credit_gate(interaction, row) == CreditGateKind::claim;
}

/** Installed sale, reward and gate identities captured for commit revalidation. */
struct RewardSaleBinding {
    build_data::vendors::InteractionGate interaction{};
    build_data::vendors::SaleGates gates{};
    std::uint32_t packageHash{};
    std::int32_t categoryIndex{build_data::vendors::kAbsentCategoryIndex};
    std::uint16_t itemIndex{};
    std::uint16_t poolIndex{build_data::rewards::kAbsent};
    std::uint16_t rankCreditRow{};
};

/**
 * Confirms that the installed faction participates in the authored reputation catalog.
 * @param vendor Installed vendor definition.
 * @return True when at least one exact turn-in names this vendor and faction.
 */
bool has_reputation_context(const build_data::vendors::Definition& vendor) noexcept {
    namespace vendors = build_data::vendors;
    if (vendor.factionHash == 0
        || vendor.factionProgressionIndex == vendors::kUnavailableFactionProgressionIndex) {
        return false;
    }
    for (std::size_t index = 0; index < vendor.saleCount; ++index) {
        vendors::ReputationSale sale{};
        if (vendors::find_reputation_sale(
                vendor.definitionHash, static_cast<std::uint16_t>(index), sale)
            && sale.factionHash == vendor.factionHash) {
            return true;
        }
    }
    return false;
}

/**
 * Finds one non-warning credit claim in the requested installed category.
 * @param vendor Installed vendor definition.
 * @param categoryIndex Requested sale category.
 * @param interaction Receives the unique claim interaction.
 * @param row Receives its saved character credit row.
 * @return False for zero, duplicate or malformed claim candidates.
 */
bool find_rank_claim_interaction(const build_data::vendors::Definition& vendor,
                                 std::int32_t categoryIndex,
                                 build_data::vendors::InteractionGate& interaction,
                                 std::uint16_t& row) noexcept {
    namespace vendors = build_data::vendors;
    interaction = {};
    row = 0;
    bool found = false;
    for (std::size_t index = 0; index < vendor.thirdCount; ++index) {
        vendors::InteractionGate candidate{};
        if (!vendors::find_interaction_gate(
                vendor.definitionHash, static_cast<std::uint16_t>(index), candidate)
            || candidate.categoryIndex != categoryIndex) {
            continue;
        }
        std::uint16_t candidateRow = 0;
        switch (credit_gate(candidate, candidateRow)) {
        case CreditGateKind::unrelated:
        case CreditGateKind::warning:
            continue;
        case CreditGateKind::malformed:
            return false;
        case CreditGateKind::claim:
            if (found) {
                return false;
            }
            found = true;
            interaction = candidate;
            row = candidateRow;
            break;
        }
    }
    return found;
}

/**
 * Resolves one exact sale-backed rank wrapper from installed content.
 * @param vendorIndex Installed vendor selector.
 * @param saleIndex Requested sale selector.
 * @param binding Receives current package, pool and gate identities.
 * @return Unrelated ordinary sale, refused candidate or prepared rank wrapper.
 */
VendorReputationDisposition resolve_reward_sale(std::uint16_t vendorIndex,
                                                std::uint16_t saleIndex,
                                                RewardSaleBinding& binding) noexcept {
    namespace vendors = build_data::vendors;
    binding = {};
    vendors::IndexEntry entry{};
    vendors::Definition vendor{};
    vendors::SaleRow sale{};
    if (!vendors::find_index(vendorIndex, entry) || !vendors::find(entry.definitionHash, vendor)) {
        return VendorReputationDisposition::notApplicable;
    }
    if (!vendors::sale_row(vendor, saleIndex, sale)) {
        return VendorReputationDisposition::refused;
    }
    vendors::SaleGates gates{};
    const bool retained = vendors::find_sale_gates(entry.definitionHash, saleIndex, gates);
    const bool freeRewardCandidate = sale.categoryIndex != vendors::kAbsentCategoryIndex
                                     && sale.costQuantity == 0
                                     && sale.costItemIndex == vendors::kAbsentCostItem;
    if (!freeRewardCandidate) {
        return retained ? VendorReputationDisposition::refused
                        : VendorReputationDisposition::notApplicable;
    }
    build_data::items::Definition package{};
    build_data::rewards::Item reward{};
    const bool context = has_reputation_context(vendor);
    const bool rewardCatalog = build_data::rewards::ready();
    const bool itemFound = build_data::find_item_definition_index(sale.itemIndex, package);
    const bool rewardFound =
        rewardCatalog && build_data::rewards::find_item(sale.itemIndex, reward);
    if (!rewardCatalog || !itemFound) {
        return retained || context ? VendorReputationDisposition::refused
                                   : VendorReputationDisposition::notApplicable;
    }
    if (!rewardFound) {
        return retained ? VendorReputationDisposition::refused
                        : VendorReputationDisposition::notApplicable;
    }
    if (package.definitionHash != reward.definitionHash) {
        return retained || context ? VendorReputationDisposition::refused
                                   : VendorReputationDisposition::notApplicable;
    }
    if (reward.poolIndex == build_data::rewards::kAbsent
        || (reward.flags & build_data::rewards::kOpenOnAcquisition) == 0) {
        return retained ? VendorReputationDisposition::refused
                        : VendorReputationDisposition::notApplicable;
    }
    if (!context || !retained || gates.categoryIndex != sale.categoryIndex) {
        return VendorReputationDisposition::refused;
    }
    build_data::vendors::InteractionGate interaction{};
    std::uint16_t creditRow = 0;
    if (!find_rank_claim_interaction(vendor, sale.categoryIndex, interaction, creditRow)) {
        return VendorReputationDisposition::refused;
    }
    binding.interaction = interaction;
    binding.gates = gates;
    binding.packageHash = package.definitionHash;
    binding.categoryIndex = sale.categoryIndex;
    binding.itemIndex = sale.itemIndex;
    binding.poolIndex = reward.poolIndex;
    binding.rankCreditRow = creditRow;
    return VendorReputationDisposition::prepared;
}

/**
 * Evaluates the claim interaction and exact sale gates against current saved state.
 * @param binding Resolved installed sale authority.
 * @return False when any saved input is missing or any complete gate refuses the claim.
 */
bool reward_gates_pass(const RewardSaleBinding& binding) noexcept {
    Family5State family{};
    AccountState account{};
    unlocks::Table banks{};
    if (!investment::store::read_family5(family) || !investment::store::read_account(account)
        || !account::valid(account)) {
        return false;
    }
    const auto selected = runtime::detail::selected_character_index(account);
    return selected < account.characterCount
           && investment::store::read_unlocks(banks, static_cast<int>(selected))
           && gate_passes(binding.interaction.condition, family, banks)
           && gate_passes(binding.gates.admission, family, banks)
           && gate_passes(binding.gates.selection, family, banks);
}

/** @return True when a prepared claim still names the same installed sale authority. */
bool same_reward_binding(const VendorRankRewardClaim& claim,
                         const RewardSaleBinding& binding) noexcept {
    return claim.packageHash == binding.packageHash && claim.categoryIndex == binding.categoryIndex
           && claim.itemIndex == binding.itemIndex && claim.poolIndex == binding.poolIndex
           && claim.interactionIndex == binding.interaction.index
           && claim.rankCreditRow == binding.rankCreditRow;
}

/**
 * Resolves a turn-in without treating another purchase using the same material as reputation.
 * @param vendorIndex Installed vendor selector.
 * @param saleIndex Installed sale selector.
 * @param award Receives the checked cost and XP; use only when prepared is returned.
 * @return Not applicable for other sales, refused for unsupported costs on a known placeholder.
 */
VendorReputationDisposition resolve_reputation_turn_in(std::uint16_t vendorIndex,
                                                       std::uint16_t saleIndex,
                                                       VendorReputationAward& award) noexcept {
    namespace vendors = build_data::vendors;
    vendors::IndexEntry entry{};
    vendors::Definition vendor{};
    vendors::SaleRow sale{};
    build_data::items::Definition sold{}, cost{};
    award = {};
    if (!vendors::find_index(vendorIndex, entry) || !vendors::find(entry.definitionHash, vendor)
        || !vendors::sale_row(vendor, saleIndex, sale)
        || !build_data::find_item_definition_index(sale.itemIndex, sold)) {
        return VendorReputationDisposition::notApplicable;
    }
    vendors::ReputationSale authored{};
    if (!vendors::find_reputation_sale(entry.definitionHash, saleIndex, authored)) {
        return vendors::is_reputation_placeholder(entry.definitionHash, sold.definitionHash)
                   ? VendorReputationDisposition::refused
                   : VendorReputationDisposition::notApplicable;
    }
    // The manifest's token value is trusted only for this exact installed sale and faction.
    if (sold.definitionHash != authored.placeholderHash
        || sale.categoryIndex != authored.categoryIndex
        || sale.costItemIndex == vendors::kAbsentCostItem
        || sale.costQuantity != authored.costQuantity
        || !build_data::find_item_definition_index(sale.costItemIndex, cost)
        || cost.definitionHash != authored.costHash || vendor.factionHash != authored.factionHash) {
        return VendorReputationDisposition::refused;
    }
    std::array<std::uint16_t, build_data::progressions::kDefinitionCapacity> slots{};
    std::size_t count = 0;
    build_data::progressions::Definition progression{};
    const auto progressionIndex = vendor.factionProgressionIndex;
    const std::int64_t experience =
        static_cast<std::int64_t>(sale.costQuantity) * authored.experiencePerUnit;
    if (progressionIndex == vendors::kUnavailableFactionProgressionIndex
        || experience > (std::numeric_limits<std::int32_t>::max)()
        || !build_data::find_progression_slots(
            build_data::progressions::Scope::character, slots, count)
        || count > slots.size()
        || std::find(
               slots.begin(), slots.begin() + static_cast<std::ptrdiff_t>(count), progressionIndex)
               == slots.begin() + static_cast<std::ptrdiff_t>(count)
        || !build_data::progressions::find(progressionIndex, progression)
        || progression.scope != build_data::progressions::Scope::character) {
        return VendorReputationDisposition::refused;
    }
    award.costHash = cost.definitionHash;
    award.costQuantity = sale.costQuantity;
    award.experience = static_cast<std::int32_t>(experience);
    award.progressionIndex = progressionIndex;
    const auto* link = vendors::find_reconstructed_rank_claim_link(entry.definitionHash);
    if (link != nullptr) {
        vendors::InteractionGate interaction{};
        if (!reconstructed_credit_gate(*link, interaction, award.rankCreditRow)) {
            return VendorReputationDisposition::refused;
        }
    }
    award.repeatLastStep = progression.repeatLastStep;
    if (award.rankCreditRow != 0) {
        std::array<build_data::progressions::Step,
                   build_data::progressions::kStepPerDefinitionCapacity>
            steps{};
        if (!build_data::progressions::steps(progressionIndex, steps, count) || count < 2
            || std::any_of(
                steps.begin(),
                steps.begin() + static_cast<std::ptrdiff_t>(count),
                [](const auto& step) { return step.cost <= 0; })) {
            return VendorReputationDisposition::refused;
        }
        award.rankStepCount = count;
        std::transform(steps.begin(),
                       steps.begin() + static_cast<std::ptrdiff_t>(count),
                       award.rankStepCosts.begin(),
                       [](const auto& step) { return step.cost; });
    }
    return VendorReputationDisposition::prepared;
}

/**
 * Charges profile stacks in their existing order without announcing an item acquisition.
 * @param account Candidate account; discard it on failure.
 * @param award Validated sale payment.
 * @return False when materials are insufficient or the resulting profile is invalid.
 */
bool charge_materials(AccountState& account, const VendorReputationAward& award) noexcept {
    std::uint32_t remaining = award.costQuantity;
    std::size_t write = 0;
    for (std::size_t read = 0; read < account.profileItemCount; ++read) {
        auto item = account.profileItems[read];
        if (item.definitionHash == award.costHash) {
            if (item.instanceSoid != 0 || item.quantity <= 0) {
                return false;
            }
            const auto charged = (std::min)(remaining, static_cast<std::uint32_t>(item.quantity));
            item.quantity -= static_cast<std::int32_t>(charged);
            remaining -= charged;
        }
        if (item.quantity != 0) {
            account.profileItems[write++] = item;
        }
    }
    std::fill(account.profileItems.begin() + static_cast<std::ptrdiff_t>(write),
              account.profileItems.end(),
              account::inventory::ProfileItem{});
    account.profileItemCount = write;
    return remaining == 0 && account::valid(account)
           && runtime::detail::valid_profile_inventory(account);
}

/**
 * Walks one captured ladder and repeats its final cost only when installed content says to.
 * @param award Prepared award carrying the installed rank costs.
 * @param experience Nonnegative progression XP.
 * @param rank Receives the number of completed positive-cost steps.
 * @return False when the captured ladder cannot define ranks.
 */
bool rank_at_experience(const VendorReputationAward& award,
                        std::int64_t experience,
                        std::int64_t& rank) noexcept {
    rank = 0;
    if (experience < 0 || award.rankStepCount < 2
        || award.rankStepCount > award.rankStepCosts.size() || award.rankStepCosts[0] <= 0) {
        return false;
    }
    std::int64_t remaining = experience;
    for (std::size_t step = 0; step < award.rankStepCount; ++step) {
        const auto cost = award.rankStepCosts[step];
        if (cost <= 0) {
            return false;
        }
        if (remaining < cost) {
            return true;
        }
        remaining -= cost;
        ++rank;
    }
    if (award.repeatLastStep) {
        rank += remaining / award.rankStepCosts[award.rankStepCount - 1];
    }
    return true;
}

/**
 * Reconstructed policy awards one claim credit per newly crossed rank, without backfilling XP.
 * @param banks Candidate unlock banks; no saved state is written here.
 * @param award Build-matched turn-in and captured rank ladder.
 * @param beforeExperience Nonnegative XP before this turn-in.
 * @return False when the ladder or counter is invalid or the complete credit would overflow.
 */
bool grant_rank_rewards(unlocks::Table& banks,
                        const VendorReputationAward& award,
                        std::int32_t beforeExperience) noexcept {
    if (award.rankStepCount == 0) {
        return true;
    }
    if (beforeExperience < 0 || award.experience <= 0) {
        return false;
    }
    const auto afterExperience = static_cast<std::int64_t>(beforeExperience) + award.experience;
    std::int64_t beforeRank = 0, afterRank = 0;
    if (!rank_at_experience(award, beforeExperience, beforeRank)
        || !rank_at_experience(award, afterExperience, afterRank) || afterRank < beforeRank) {
        return false;
    }
    const auto count = afterRank - beforeRank;
    auto& credits = banks.characterObjectValues[award.rankCreditRow];
    if (credits < 0 || count > (std::numeric_limits<std::int32_t>::max)() - credits) {
        return false;
    }
    credits += static_cast<std::int32_t>(count);
    return true;
}

} // namespace

/**
 * Captures an affordable turn-in without writing inventory or progression.
 * @param vendorIndex Installed vendor selector.
 * @param saleIndex Installed sale selector.
 * @param mutation Receives the before-image; cleared on failure.
 * @return Whether this sale is a prepared, refused or unrelated reputation action.
 */
VendorReputationDisposition prepare_vendor_reputation(std::uint16_t vendorIndex,
                                                      std::uint16_t saleIndex,
                                                      PendingVendorReputation& mutation) noexcept {
    mutation = {};
    VendorReputationAward award{};
    const auto disposition = resolve_reputation_turn_in(vendorIndex, saleIndex, award);
    if (disposition != VendorReputationDisposition::prepared) {
        return disposition;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    AccountState account{};
    unlocks::Table banks{};
    if (!investment::store::read_account(account) || !account::valid(account)
        || !runtime::detail::valid_profile_inventory(account)) {
        return VendorReputationDisposition::refused;
    }
    const auto selected = runtime::detail::selected_character_index(account);
    if (selected >= account.characterCount || account.primarySoid == 0
        || !investment::store::read_unlocks(banks, static_cast<int>(selected))) {
        return VendorReputationDisposition::refused;
    }
    const auto before = banks.characterProgressions[award.progressionIndex];
    if (before[kExperienceLane] < 0
        || before[kExperienceLane]
               > (std::numeric_limits<std::int32_t>::max)() - award.experience) {
        return VendorReputationDisposition::refused;
    }
    AccountState after = account;
    const auto beforeCredits = banks.characterObjectValues[award.rankCreditRow];
    if (!charge_materials(after, award)
        || !grant_rank_rewards(banks, award, before[kExperienceLane])) {
        return VendorReputationDisposition::refused;
    }
    mutation.beforeItems = account.profileItems;
    mutation.beforeItemCount = account.profileItemCount;
    mutation.beforeProgression = before;
    mutation.beforeRankCredits = beforeCredits;
    mutation.award = award;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = account.characters[selected].soid;
    mutation.characterIndex = selected;
    mutation.vendorIndex = vendorIndex;
    mutation.saleIndex = saleIndex;
    mutation.prepared = true;
    return VendorReputationDisposition::prepared;
}

/**
 * Materials, XP and claim credits share the caller's response transaction.
 * @param mutation Before-image to consume, including on refusal.
 * @return False for stale state, changed content or any failed grant/write; all changes roll back.
 */
bool commit_vendor_reputation(PendingVendorReputation& mutation) noexcept {
    const runtime::detail::PendingConsumption consume(mutation);
    VendorReputationAward award{};
    if (!mutation.prepared
        || resolve_reputation_turn_in(mutation.vendorIndex, mutation.saleIndex, award)
               != VendorReputationDisposition::prepared
        || award != mutation.award) {
        return false;
    }
    investment::store::Transaction transaction;
    AccountState account{};
    unlocks::Table banks{};
    if (!transaction.ready() || !investment::store::read_account(account)
        || !account::valid(account) || account.primarySoid != mutation.accountSoid
        || !runtime::detail::valid_profile_inventory(account)
        || mutation.characterIndex >= account.characterCount
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid
        || !account.characters[mutation.characterIndex].selected
        || !runtime::detail::same_profile_inventory(
            account, mutation.beforeItems, mutation.beforeItemCount)
        || !investment::store::read_unlocks(banks, static_cast<int>(mutation.characterIndex))) {
        return false;
    }
    auto& progression = banks.characterProgressions[award.progressionIndex];
    if (progression != mutation.beforeProgression || progression[kExperienceLane] < 0
        || (award.rankStepCount != 0
            && banks.characterObjectValues[award.rankCreditRow] != mutation.beforeRankCredits)
        || progression[kExperienceLane]
               > (std::numeric_limits<std::int32_t>::max)() - award.experience
        || !charge_materials(account, award)
        || !grant_rank_rewards(banks, award, progression[kExperienceLane])) {
        return false;
    }
    progression[kExperienceLane] += award.experience;
    return investment::store::write_account(account)
           && investment::store::write_unlocks(banks, static_cast<int>(mutation.characterIndex))
           && transaction.commit();
}

/**
 * Resolves a supported rowless reply through the same settlement as its package sale.
 * @param vendorIndex Installed vendor selector.
 * @param interactionIndex Rowless interaction selector.
 * @param replyIndex Reply within that interaction.
 * @param mutation Receives the prepared grant; cleared on refusal.
 * @return Recognized but unsupported replies refuse rather than falling through to a free grant.
 */
VendorReputationDisposition
prepare_vendor_rank_reward_interaction(std::uint16_t vendorIndex,
                                       std::uint16_t interactionIndex,
                                       std::uint16_t replyIndex,
                                       PendingRecordRewardGrant& mutation) noexcept {
    mutation = {};
    build_data::vendors::IndexEntry entry{};
    if (!build_data::vendors::find_index(vendorIndex, entry)) {
        return VendorReputationDisposition::notApplicable;
    }
    const auto* link =
        build_data::vendors::find_reconstructed_rank_claim_link(entry.definitionHash);
    if (link == nullptr || link->interactionIndex != interactionIndex) {
        return VendorReputationDisposition::notApplicable;
    }
    if (replyIndex != kAcceptRewardReply) {
        return VendorReputationDisposition::refused;
    }
    const auto disposition =
        prepare_vendor_rank_reward_sale(vendorIndex, link->saleIndex, mutation);
    if (disposition != VendorReputationDisposition::prepared
        || mutation.vendorReward.interactionIndex != link->interactionIndex
        || mutation.vendorReward.categoryIndex != link->categoryIndex) {
        mutation = {};
        return VendorReputationDisposition::refused;
    }
    return VendorReputationDisposition::prepared;
}

/**
 * A reward sale binds its rank credit to the installed package's prepared grant.
 * @param vendorIndex Installed vendor selector.
 * @param saleIndex Requested package sale.
 * @param mutation Receives the prepared grant; cleared on refusal.
 * @param refusal Receives the first failed guard, when requested.
 * @return Unrelated sales are not applicable; unsupported or unaffordable rewards are refused.
 */
VendorReputationDisposition prepare_vendor_rank_reward_sale(std::uint16_t vendorIndex,
                                                            std::uint16_t saleIndex,
                                                            PendingRecordRewardGrant& mutation,
                                                            const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "reward_binding";
    mutation = {};
    const std::lock_guard lock(investment::store::g_mutex);
    RewardSaleBinding binding{};
    const auto disposition = resolve_reward_sale(vendorIndex, saleIndex, binding);
    if (disposition != VendorReputationDisposition::prepared) {
        return disposition;
    }
    reason = "rank_credit";
    std::int32_t credits = 0;
    if (!investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, binding.rankCreditRow, credits)
        || credits <= 0) {
        return VendorReputationDisposition::refused;
    }
    reason = "reward_binding";
    if (!reward_gates_pass(binding)) {
        return VendorReputationDisposition::refused;
    }
    if (!prepare_item_reward(binding.itemIndex, 1, mutation, &reason)) {
        mutation = {};
        return VendorReputationDisposition::refused;
    }
    reason = nullptr;
    mutation.vendorReward.beforeCredits = credits;
    mutation.vendorReward.vendorIndex = vendorIndex;
    mutation.vendorReward.saleIndex = saleIndex;
    mutation.vendorReward.packageHash = binding.packageHash;
    mutation.vendorReward.categoryIndex = binding.categoryIndex;
    mutation.vendorReward.itemIndex = binding.itemIndex;
    mutation.vendorReward.poolIndex = binding.poolIndex;
    mutation.vendorReward.interactionIndex = binding.interaction.index;
    mutation.vendorReward.rankCreditRow = binding.rankCreditRow;
    return VendorReputationDisposition::prepared;
}

/**
 * Claim credit and the installed sale must still match before publication.
 * Caller holds the investment-store lock across this check and the reward preview.
 * @param claim Prepared claim state; zero credits mean no vendor claim.
 * @return False for a stale credit or changed package gate.
 */
bool vendor_rank_reward_current(const VendorRankRewardClaim& claim) noexcept {
    if (claim.beforeCredits == 0) {
        return true;
    }
    std::int32_t current = 0;
    RewardSaleBinding binding{};
    if (claim.beforeCredits < 0
        || resolve_reward_sale(claim.vendorIndex, claim.saleIndex, binding)
               != VendorReputationDisposition::prepared
        || !same_reward_binding(claim, binding) || !reward_gates_pass(binding)
        || !investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, claim.rankCreditRow, current)
        || current != claim.beforeCredits) {
        return false;
    }
    return true;
}

} // namespace sunrise::state
