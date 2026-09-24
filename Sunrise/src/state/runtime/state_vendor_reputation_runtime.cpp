#include "state_vendor_reputation_runtime.h"

#include <algorithm>
#include <limits>

#include "../build_data/progressions/progression_catalog.h"
#include "../build_data/rewards/reward_catalog.h"
#include "../build_data/vendors/reputation_sale_catalog.h"
#include "../build_data/vendors/vendor_catalog.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {

/** Build-86657 VALUE[888] and VALUE[906] map to these character-object value rows. */
constexpr std::uint16_t kCrucibleRewardValueRow = 45, kGunsmithRewardValueRow = 49;

/** Native progression level walks read experience from lane zero. */
constexpr std::size_t kExperienceLane = 0;

/** Checked build-86657 claim bindings; the installed reward catalog resolves payout. */
struct RewardRule {
    std::uint32_t vendorHash;
    std::uint16_t interaction;
    std::int32_t category;
    std::uint16_t rewardValueRow;
    std::uint16_t saleIndex{};
    std::uint32_t packageHash{};
    /** Only the Vanguard and Crucible sales require FLAG[5901] and VALUE[465]. */
    bool requiresSelectionGates{true};
};
/** Build-86657 Vanguard rank package item hash. */
constexpr std::uint32_t kVanguardPackageHash = 2746484552U;
/** Build-86657 Crucible rank package item hash. */
constexpr std::uint32_t kCruciblePackageHash = 3289621657U;
/** Build-86657 Gunsmith rank package item hash. */
constexpr std::uint32_t kGunsmithPackageHash = 2422825785U;
/** Vendor, interaction, category, saved counter, package sale and hash from build 86657. */
constexpr std::array<RewardRule, 3> kRewardRules{{
    // Zavala's current package previews Vanguard gear.
    {69482069U, 40, 3, kVanguardRewardValueRow, 93, kVanguardPackageHash},
    // Shaxx's current package previews Crucible gear.
    {3603221665U, 28, 10, kCrucibleRewardValueRow, 96, kCruciblePackageHash},
    // Banshee's sale has no Vanguard/Crucible selection gates.
    {672118013U, 35, 8, kGunsmithRewardValueRow, 16, kGunsmithPackageHash, false},
}};
/** Reply zero completes the supported normal reward interactions. */
constexpr std::uint16_t kAcceptRewardReply = 0;
/** FLAG[5901] selects the current Vanguard and Crucible packages over their older variants. */
constexpr std::uint16_t kCurrentPackageFlag = 5901;
/** The normal reward interaction requires VALUE[465] >= 20. */
constexpr std::uint16_t kRewardLevelSlot = 465;
/** The normal reward interaction excludes levels below this content threshold. */
constexpr std::int32_t kMinimumRewardLevel = 20;

/**
 * Resolves vendor identity independently of request selectors.
 * @param vendorIndex Installed vendor selector.
 * @return Checked binding, including unsupported payouts, or null for unrelated vendors.
 */
const RewardRule* reward_rule(std::uint16_t vendorIndex) noexcept {
    build_data::vendors::IndexEntry entry{};
    if (!build_data::vendors::find_index(vendorIndex, entry)) {
        return nullptr;
    }
    const auto found =
        std::find_if(kRewardRules.begin(), kRewardRules.end(), [&](const auto& rule) {
            return rule.vendorHash == entry.definitionHash;
        });
    return found == kRewardRules.end() ? nullptr : &*found;
}

/**
 * Accept only the supported auto-opening package and explicit evaluated gates.
 * Caller holds the investment-store lock while reading the current gates.
 * @param rule Checked vendor binding.
 * @param saleIndex Requested sale, retained for commit revalidation.
 * @return True when saved gates and the installed package binding agree.
 */
bool reward_binding_current(const RewardRule& rule, std::uint16_t saleIndex) noexcept {
    namespace vendors = build_data::vendors;
    vendors::Definition vendor{};
    vendors::SaleRow sale{};
    build_data::items::Definition package{};
    build_data::rewards::Item reward{};
    if (saleIndex != rule.saleIndex || !vendors::find(rule.vendorHash, vendor)
        || !vendors::sale_row(vendor, saleIndex, sale) || sale.categoryIndex != rule.category
        || sale.costQuantity != 0
        || !build_data::find_item_definition_index(sale.itemIndex, package)
        || package.definitionHash != rule.packageHash
        || !build_data::rewards::find_item(sale.itemIndex, reward)
        || reward.definitionHash != package.definitionHash
        || reward.poolIndex == build_data::rewards::kAbsent
        || (reward.flags & build_data::rewards::kOpenOnAcquisition) == 0) {
        return false;
    }
    if (!rule.requiresSelectionGates) {
        return true;
    }
    Family5State family{};
    if (!investment::store::read_family5(family)) {
        return false;
    }
    bool packageEnabled = false, levelMet = false;
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        const auto& flag = family.flags[index];
        if (flag.slot == kCurrentPackageFlag) {
            packageEnabled = flag.value == unlocks::kFlagSet;
        }
    }
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        const auto& value = family.values[index];
        if (value.slot == kRewardLevelSlot) {
            levelMet = value.value >= kMinimumRewardLevel;
        }
    }
    return packageEnabled && levelMet;
}

/**
 * Resolves a turn-in without treating another purchase using the same material as reputation.
 * @param vendorIndex Installed vendor selector.
 * @param saleIndex Installed sale selector.
 * @param award Receives the checked cost and XP; use only when prepared is returned.
 * @return Not applicable for other sales, refused for unsupported costs on a known placeholder.
 */
VendorReputationDisposition resolve_award(std::uint16_t vendorIndex,
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
    const auto* reward = reward_rule(vendorIndex);
    award.rewardValueRow = reward != nullptr ? reward->rewardValueRow : 0;
    award.repeatLastStep = progression.repeatLastStep;
    if (award.rewardValueRow != 0) {
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
    auto& credits = banks.characterObjectValues[award.rewardValueRow];
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
    const auto disposition = resolve_award(vendorIndex, saleIndex, award);
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
    const auto beforeCredits = banks.characterObjectValues[award.rewardValueRow];
    if (!charge_materials(after, award)
        || !grant_rank_rewards(banks, award, before[kExperienceLane])) {
        return VendorReputationDisposition::refused;
    }
    mutation.beforeItems = account.profileItems;
    mutation.beforeItemCount = account.profileItemCount;
    mutation.beforeProgression = before;
    mutation.beforeRewardCredits = beforeCredits;
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
        || resolve_award(mutation.vendorIndex, mutation.saleIndex, award)
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
            && banks.characterObjectValues[award.rewardValueRow] != mutation.beforeRewardCredits)
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
 * Reward categories cannot fall through to a free package acquisition.
 * @param vendorIndex Installed vendor selector.
 * @param categoryIndex Installed sale category.
 * @return True for a mapped rank-reward category, including unsupported payout vendors.
 */
bool is_vendor_reward_category(std::uint16_t vendorIndex, std::int32_t categoryIndex) noexcept {
    build_data::vendors::IndexEntry entry{};
    if (!build_data::vendors::find_index(vendorIndex, entry)) {
        return false;
    }
    return std::any_of(kRewardRules.begin(), kRewardRules.end(), [&](const auto& rule) {
        return rule.vendorHash == entry.definitionHash && rule.category == categoryIndex;
    });
}

/**
 * Resolves a supported rowless reply through the same settlement as its package sale.
 * @param vendorIndex Installed vendor selector.
 * @param interactionIndex Rowless interaction selector.
 * @param replyIndex Reply within that interaction.
 * @param mutation Receives the prepared grant; cleared on refusal.
 * @return Recognized but unsupported replies refuse rather than falling through to a free grant.
 */
VendorReputationDisposition prepare_vendor_reward(std::uint16_t vendorIndex,
                                                  std::uint16_t interactionIndex,
                                                  std::uint16_t replyIndex,
                                                  PendingRecordRewardGrant& mutation) noexcept {
    mutation = {};
    const auto* rule = reward_rule(vendorIndex);
    if (rule == nullptr || rule->interaction != interactionIndex) {
        return VendorReputationDisposition::notApplicable;
    }
    if (replyIndex != kAcceptRewardReply) {
        return VendorReputationDisposition::refused;
    }
    return prepare_vendor_reward_sale(vendorIndex, rule->saleIndex, mutation);
}

/**
 * A reward sale binds its rank credit to the installed package's prepared grant.
 * @param vendorIndex Installed vendor selector.
 * @param saleIndex Requested package sale.
 * @param mutation Receives the prepared grant; cleared on refusal.
 * @param refusal Receives the first failed guard, when requested.
 * @return Unrelated sales are not applicable; unsupported or unaffordable rewards are refused.
 */
VendorReputationDisposition prepare_vendor_reward_sale(std::uint16_t vendorIndex,
                                                       std::uint16_t saleIndex,
                                                       PendingRecordRewardGrant& mutation,
                                                       const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "vendor_rule";
    mutation = {};
    const auto* rule = reward_rule(vendorIndex);
    build_data::vendors::Definition vendor{};
    build_data::vendors::SaleRow sale{};
    if (rule == nullptr) {
        return VendorReputationDisposition::notApplicable;
    }
    reason = "vendor_sale";
    if (!build_data::vendors::find(rule->vendorHash, vendor)
        || !build_data::vendors::sale_row(vendor, saleIndex, sale)) {
        return VendorReputationDisposition::refused;
    }
    if (sale.categoryIndex != rule->category) {
        return VendorReputationDisposition::notApplicable;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    reason = "reward_binding";
    if (!reward_binding_current(*rule, saleIndex)) {
        return VendorReputationDisposition::refused;
    }
    reason = "rank_credit";
    std::int32_t credits = 0;
    if (!investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, rule->rewardValueRow, credits)
        || credits <= 0) {
        return VendorReputationDisposition::refused;
    }
    if (!prepare_item_reward(sale.itemIndex, 1, mutation, &reason)) {
        mutation = {};
        return VendorReputationDisposition::refused;
    }
    reason = nullptr;
    mutation.vendorReward = {credits, vendorIndex, saleIndex, rule->rewardValueRow};
    return VendorReputationDisposition::prepared;
}

/**
 * Claim credit and the installed sale must still match before publication.
 * Caller holds the investment-store lock across this check and the reward preview.
 * @param claim Prepared claim state; zero credits mean no vendor claim.
 * @return False for a stale credit or changed package gate.
 */
bool vendor_reward_current(const VendorRewardClaim& claim) noexcept {
    if (claim.beforeCredits == 0) {
        return true;
    }
    std::int32_t current = 0;
    const auto* rule = reward_rule(claim.vendorIndex);
    if (claim.beforeCredits < 0 || rule == nullptr || claim.rewardValueRow != rule->rewardValueRow
        || !reward_binding_current(*rule, claim.saleIndex)
        || !investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, claim.rewardValueRow, current)
        || current != claim.beforeCredits) {
        return false;
    }
    return true;
}

} // namespace sunrise::state
