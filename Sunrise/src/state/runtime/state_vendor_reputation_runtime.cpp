#include "state_vendor_reputation_runtime.h"

#include <algorithm>
#include <limits>

#include "../build_data/progressions/progression_catalog.h"
#include "../build_data/vendors/vendor_catalog.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"
#include "vendor_reward_pool.h"

namespace sunrise::state {
namespace {

/** Build-86657 faction tokenValues apply only to the matching vendor sale placeholder. */
struct ReputationRule {
    std::uint32_t vendorHash;
    std::uint32_t placeholderHash;
    std::uint32_t costHash;
    std::uint16_t progressionIndex;
    std::int32_t experiencePerUnit;
    std::uint16_t rewardValueRow{};
    bool repeatLastStep{};
};

/** Build-86657 VALUE[888] and VALUE[906] map to these character-object value rows. */
constexpr std::uint16_t kCrucibleRewardValueRow = 45, kGunsmithRewardValueRow = 49;
/** Build-86657 progressions 49, 55 and 62 repeat their final installed step. */
constexpr bool kRepeatFactionRankStep = true;

/** Vendor/item hashes, character progression indices and XP per unit from build 86657. */
constexpr std::array<ReputationRule, 7> kReputationRules{{
    // Banshee: Gunsmith Rewards charges Gunsmith Materials for Gunsmith progression.
    {672118013U, 3831705402U, 685157383U, 55, 30, kGunsmithRewardValueRow, kRepeatFactionRankStep},
    // Banshee: the same placeholder also accepts Weapon Telemetry at its own XP rate.
    {672118013U, 3831705402U, 685157381U, 55, 25, kGunsmithRewardValueRow, kRepeatFactionRankStep},
    // Zavala: Vanguard Tactician Rewards charges Vanguard Tactician Tokens.
    {69482069U, 3987308529U, 3899548068U, 62, 100, kVanguardRewardValueRow, kRepeatFactionRankStep},
    // Shaxx: Crucible Rewards charges Crucible Tokens, not Valor or Glory points.
    {3603221665U, 265113466U, 183980811U, 49, 100, kCrucibleRewardValueRow, kRepeatFactionRankStep},
    // Devrim: EDZ token turn-ins use a different placeholder from destination materials.
    {396892126U, 61430328U, 2640973641U, 52, 100},
    // Devrim: destination-material turn-ins accept Dusklight Shards.
    {396892126U, 1317670974U, 950899352U, 52, 50},
    // Devrim: the same material placeholder accepts Dusklight Crystals at their own XP rate.
    {396892126U, 1317670974U, 478751073U, 52, 250},
}};

/** Native progression level walks read experience from lane zero. */
constexpr std::size_t kExperienceLane = 0;

/** Checked build-86657 claim bindings; a missing pool leaves payout unsupported. */
struct RewardRule {
    std::uint32_t vendorHash;
    std::uint16_t interaction;
    std::int32_t category;
    std::uint16_t rewardValueRow;
    std::uint16_t saleIndex{};
    std::uint32_t packageHash{};
    const vendor_rewards::Pool* pool{};
    /** Only the Vanguard and Crucible sales require FLAG[5901] and VALUE[465]. */
    bool requiresSelectionGates{true};
};
/** Vendor, interaction, category, saved counter, package sale and hash from build 86657. */
constexpr std::array<RewardRule, 3> kRewardRules{{
    // Zavala's current package previews Vanguard gear.
    {69482069U, 40, 3, kVanguardRewardValueRow, 93, 2746484552U, &vendor_rewards::kVanguardPool},
    // Shaxx's current package previews Crucible gear.
    {3603221665U, 28, 10, kCrucibleRewardValueRow, 96, 3289621657U, &vendor_rewards::kCruciblePool},
    // Banshee's sale has empty selection gates and uses the shared weapon pool without armour.
    {672118013U,
     35,
     8,
     kGunsmithRewardValueRow,
     16,
     2422825785U,
     &vendor_rewards::kGunsmithPool,
     false},
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
 * Accept only the supported package and explicit evaluated gates; do not guess missing values.
 * @param rule Checked vendor binding.
 * @param saleIndex Requested sale, retained for commit revalidation.
 * @return True when saved gates and the installed package binding agree.
 */
bool reward_binding_current(const RewardRule& rule, std::uint16_t saleIndex) noexcept {
    namespace vendors = build_data::vendors;
    vendors::Definition vendor{};
    vendors::SaleRow sale{};
    build_data::items::Definition package{};
    if (rule.pool == nullptr || saleIndex != rule.saleIndex
        || !vendors::find(rule.vendorHash, vendor) || !vendors::sale_row(vendor, saleIndex, sale)
        || sale.categoryIndex != rule.category || sale.costQuantity != 0
        || !build_data::find_item_definition_index(sale.itemIndex, package)
        || package.definitionHash != rule.packageHash) {
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
 * Supported rewards must be installed instanced gear, not packages, cosmetics or quests.
 * @param hash Candidate preview item hash.
 * @return True when the installed item and detail rows agree on supported gear.
 */
bool reward_item_supported(std::uint32_t hash) noexcept {
    build_data::items::Definition item{};
    build_data::items::details::Definition detail{};
    return build_data::find_item_definition_hash(hash, item)
           && item.questInitialization.scope == build_data::items::QuestInitialization::Scope::none
           && build_data::find_configured_item_detail(item.definitionIndex, detail)
           && detail.definitionHash == hash && detail.definitionIndex == item.definitionIndex
           && detail.bucketId == item.bucketId && detail.equipmentSlot.has_value()
           && detail.instancedDefinitionState
                  == build_data::items::details::InstancedDefinitionState::instanced;
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
    bool recognized = false;
    for (const auto& rule : kReputationRules) {
        if (rule.vendorHash != entry.definitionHash
            || rule.placeholderHash != sold.definitionHash) {
            continue;
        }
        recognized = true;
        if (sale.costItemIndex == vendors::kAbsentCostItem || sale.costQuantity == 0
            || !build_data::find_item_definition_index(sale.costItemIndex, cost)
            || cost.definitionHash != rule.costHash) {
            continue;
        }
        std::array<std::uint16_t, build_data::progressions::kDefinitionCapacity> slots{};
        std::size_t count = 0;
        const std::int64_t experience =
            static_cast<std::int64_t>(sale.costQuantity) * rule.experiencePerUnit;
        if (experience > (std::numeric_limits<std::int32_t>::max)()
            || !build_data::find_progression_slots(
                build_data::progressions::Scope::character, slots, count)
            || count > slots.size()
            || std::find(slots.begin(),
                         slots.begin() + static_cast<std::ptrdiff_t>(count),
                         rule.progressionIndex)
                   == slots.begin() + static_cast<std::ptrdiff_t>(count)) {
            return VendorReputationDisposition::refused;
        }
        award.costHash = cost.definitionHash;
        award.costQuantity = sale.costQuantity;
        award.experience = static_cast<std::int32_t>(experience);
        award.progressionIndex = rule.progressionIndex;
        award.rewardValueRow = rule.rewardValueRow;
        award.repeatLastStep = rule.repeatLastStep;
        if (rule.rewardValueRow != 0) {
            std::array<build_data::progressions::Step,
                       build_data::progressions::kStepPerDefinitionCapacity>
                steps{};
            if (!build_data::progressions::steps(rule.progressionIndex, steps, count) || count < 2
                || steps[0].cost != 0
                || std::any_of(
                    steps.begin() + 1,
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
    return recognized ? VendorReputationDisposition::refused
                      : VendorReputationDisposition::notApplicable;
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
 * @param rank Receives the number of completed steps, including the zero-cost first step.
 * @return False when the captured ladder cannot define ranks.
 */
bool rank_at_experience(const VendorReputationAward& award,
                        std::int64_t experience,
                        std::int64_t& rank) noexcept {
    rank = 0;
    if (experience < 0 || award.rankStepCount < 2
        || award.rankStepCount > award.rankStepCosts.size() || award.rankStepCosts[0] != 0) {
        return false;
    }
    std::int64_t remaining = experience;
    for (std::size_t step = 0; step < award.rankStepCount; ++step) {
        const auto cost = award.rankStepCosts[step];
        if (step != 0 && cost <= 0) {
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
 * @param random Server-generated selection value.
 * @param mutation Receives the prepared grant; cleared on refusal.
 * @return Recognized but unsupported replies refuse rather than falling through to a free grant.
 */
VendorReputationDisposition prepare_vendor_reward(std::uint16_t vendorIndex,
                                                  std::uint16_t interactionIndex,
                                                  std::uint16_t replyIndex,
                                                  std::uint32_t random,
                                                  PendingItemAcquisition& mutation) noexcept {
    mutation = {};
    const auto* rule = reward_rule(vendorIndex);
    if (rule == nullptr || rule->interaction != interactionIndex) {
        return VendorReputationDisposition::notApplicable;
    }
    if (replyIndex != kAcceptRewardReply || rule->pool == nullptr) {
        return VendorReputationDisposition::refused;
    }
    return prepare_vendor_reward_sale(vendorIndex, rule->saleIndex, random, mutation);
}

/**
 * A reward sale consumes its own faction credit only together with the granted gear.
 * @param vendorIndex Installed vendor selector.
 * @param saleIndex Requested package sale.
 * @param random Server-generated selection value; never supplied by the client.
 * @param mutation Receives the prepared grant; cleared on refusal.
 * @return Unrelated sales are not applicable; unsupported or unaffordable rewards are refused.
 */
VendorReputationDisposition prepare_vendor_reward_sale(std::uint16_t vendorIndex,
                                                       std::uint16_t saleIndex,
                                                       std::uint32_t random,
                                                       PendingItemAcquisition& mutation) noexcept {
    mutation = {};
    const auto* rule = reward_rule(vendorIndex);
    build_data::vendors::Definition vendor{};
    build_data::vendors::SaleRow sale{};
    if (rule == nullptr) {
        return VendorReputationDisposition::notApplicable;
    }
    if (!build_data::vendors::find(rule->vendorHash, vendor)
        || !build_data::vendors::sale_row(vendor, saleIndex, sale)) {
        return VendorReputationDisposition::refused;
    }
    if (sale.categoryIndex != rule->category) {
        return VendorReputationDisposition::notApplicable;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    AccountState account{};
    std::int32_t credits = 0;
    if (!reward_binding_current(*rule, saleIndex) || !investment::store::read_account(account)
        || !account::valid(account) || !runtime::detail::valid_profile_inventory(account)
        || !investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, rule->rewardValueRow, credits)
        || credits <= 0) {
        return VendorReputationDisposition::refused;
    }
    const auto selected = runtime::detail::selected_character_index(account);
    if (selected >= account.characterCount) {
        return VendorReputationDisposition::refused;
    }
    const auto armour =
        vendor_rewards::armour(*rule->pool, account.characters[selected].characterClass);
    std::array<std::uint32_t, vendor_rewards::kCandidateCapacity> candidates{};
    if (rule->pool->weapons.size() + armour.size() > candidates.size()) {
        return VendorReputationDisposition::refused;
    }
    std::size_t count = 0;
    for (const auto pool : {rule->pool->weapons, armour}) {
        for (const auto hash : pool) {
            if (reward_item_supported(hash)) {
                candidates[count++] = hash;
            }
        }
    }
    // Reconstructed policy chooses one installed gear row; retail weights and extras are unknown.
    if (count == 0
        || !runtime::detail::finalize_item_acquisition(
            account, account, candidates[random % count], false, {.direct = true}, mutation)) {
        mutation = {};
        return VendorReputationDisposition::refused;
    }
    mutation.vendorReward = {credits, vendorIndex, saleIndex, rule->rewardValueRow};
    return VendorReputationDisposition::prepared;
}

/**
 * Hold the store lock; claim credit and content gates must still match before publication.
 * @param mutation Prepared item acquisition with optional vendor claim state.
 * @return False for a stale credit, changed gate or an item outside the selected class's pool.
 */
bool vendor_reward_current(const PendingItemAcquisition& mutation) noexcept {
    const auto& claim = mutation.vendorReward;
    if (claim.beforeCredits == 0) {
        return true;
    }
    std::int32_t current = 0;
    const auto* rule = reward_rule(claim.vendorIndex);
    if (claim.beforeCredits < 0 || !mutation.directGrant || rule == nullptr
        || claim.rewardValueRow != rule->rewardValueRow
        || !reward_binding_current(*rule, claim.saleIndex)
        || !investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, claim.rewardValueRow, current)
        || current != claim.beforeCredits
        || !reward_item_supported(mutation.acquiredDefinitionHash)) {
        return false;
    }
    const auto armour =
        vendor_rewards::armour(*rule->pool, mutation.beforeCharacter.characterClass);
    const auto weapons = rule->pool->weapons;
    return std::find(weapons.begin(), weapons.end(), mutation.acquiredDefinitionHash)
               != weapons.end()
           || std::find(armour.begin(), armour.end(), mutation.acquiredDefinitionHash)
                  != armour.end();
}

} // namespace sunrise::state
