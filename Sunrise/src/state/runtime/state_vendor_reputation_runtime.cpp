#include "state_vendor_reputation_runtime.h"

#include <algorithm>
#include <limits>

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
    std::int32_t rankCost{};
};

/** Build-86657 VALUE[888] and VALUE[906] map to these character-object value rows. */
constexpr std::uint16_t kCrucibleRewardValueRow = 45, kGunsmithRewardValueRow = 49;
/** Build-86657 Vanguard and Crucible use 2000-XP steps and repeat the last step. */
constexpr std::int32_t kTokenRankCost = 2000;
/** Build-86657 Gunsmith uses 3000-XP steps and repeats the last step. */
constexpr std::int32_t kGunsmithRankCost = 3000;

/** Vendor/item hashes, character progression indices and XP per unit from build 86657. */
constexpr std::array<ReputationRule, 7> kReputationRules{{
    // Banshee: Gunsmith Rewards charges Gunsmith Materials for Gunsmith progression.
    {672118013U, 3831705402U, 685157383U, 55, 30, kGunsmithRewardValueRow, kGunsmithRankCost},
    // Banshee: the same placeholder also accepts Weapon Telemetry at its own XP rate.
    {672118013U, 3831705402U, 685157381U, 55, 25, kGunsmithRewardValueRow, kGunsmithRankCost},
    // Zavala: Vanguard Tactician Rewards charges Vanguard Tactician Tokens.
    {69482069U, 3987308529U, 3899548068U, 62, 100, kVanguardRewardValueRow, kTokenRankCost},
    // Shaxx: Crucible Rewards charges Crucible Tokens, not Valor or Glory points.
    {3603221665U, 265113466U, 183980811U, 49, 100, kCrucibleRewardValueRow, kTokenRankCost},
    // Devrim: EDZ token turn-ins use a different placeholder from destination materials.
    {396892126U, 61430328U, 2640973641U, 52, 100},
    // Devrim: destination-material turn-ins accept Dusklight Shards.
    {396892126U, 1317670974U, 950899352U, 52, 50},
    // Devrim: the same material placeholder accepts Dusklight Crystals at their own XP rate.
    {396892126U, 1317670974U, 478751073U, 52, 250},
}};

/** Native progression level walks read experience from lane zero. */
constexpr std::size_t kExperienceLane = 0;

/** Build-86657 vendor hashes, normal claim interactions and reward categories. */
struct RewardRule {
    std::uint32_t vendorHash;
    std::uint16_t interaction;
    std::int32_t category;
};
/** Only these build-86657 normal claim interactions own the mapped reward categories. */
constexpr std::array<RewardRule, 3> kRewardRules{{
    {69482069U, 40, 3},    // Zavala: Accept Reward.
    {3603221665U, 28, 10}, // Shaxx: Accept Reward; payout not yet supported.
    {672118013U, 35, 8},   // Banshee: Accept Reward; payout not yet supported.
}};
/** Only Zavala's current package has a supported payout binding. */
constexpr std::uint32_t kZavalaHash = 69482069U;
/** Build-86657 current Vanguard package occupies sale row 93. */
constexpr std::uint16_t kVanguardPackageSale = 93;
/** The package opens on acquisition and previews vendor 1016620613. */
constexpr std::uint32_t kVanguardPackageHash = 2746484552U;
/** Reply zero completes Zavala's normal reward interaction. */
constexpr std::uint16_t kAcceptRewardReply = 0;
/** FLAG[5901] selects the supported Vanguard package instead of the older one. */
constexpr std::uint16_t kVanguardPackageFlag = 5901;
/** The normal reward interaction requires VALUE[465] >= 20. */
constexpr std::uint16_t kRewardLevelSlot = 465;
/** The normal reward interaction excludes levels below this content threshold. */
constexpr std::int32_t kMinimumRewardLevel = 20;

/**
 * Accept only the supported package and explicit evaluated gates; do not guess missing values.
 * @param vendorIndex Installed vendor selector.
 * @return True when the saved evaluated gates and installed package binding agree.
 */
bool reward_binding_current(std::uint16_t vendorIndex) noexcept {
    namespace vendors = build_data::vendors;
    vendors::IndexEntry entry{};
    vendors::Definition vendor{};
    vendors::SaleRow sale{};
    build_data::items::Definition package{};
    Family5State family{};
    if (!vendors::find_index(vendorIndex, entry) || entry.definitionHash != kZavalaHash
        || !vendors::find(entry.definitionHash, vendor)
        || !vendors::sale_row(vendor, kVanguardPackageSale, sale)
        || sale.categoryIndex != kRewardRules.front().category || sale.costQuantity != 0
        || !build_data::find_item_definition_index(sale.itemIndex, package)
        || package.definitionHash != kVanguardPackageHash
        || !investment::store::read_family5(family)) {
        return false;
    }
    bool packageEnabled = false, levelMet = false;
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        const auto& flag = family.flags[index];
        if (flag.slot == kVanguardPackageFlag) {
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
        award = {cost.definitionHash,
                 sale.costQuantity,
                 static_cast<std::int32_t>(experience),
                 rule.progressionIndex,
                 rule.rewardValueRow,
                 rule.rankCost};
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
 * Reconstructed policy awards one claim credit per newly crossed rank, without backfilling XP.
 * @param banks Candidate unlock banks; no saved state is written here.
 * @param award Build-matched turn-in and repeating rank rule.
 * @param beforeExperience Nonnegative XP before this turn-in.
 * @return False when the counter is negative or the complete credit would overflow.
 */
bool grant_rank_rewards(unlocks::Table& banks,
                        const VendorReputationAward& award,
                        std::int32_t beforeExperience) noexcept {
    if (award.rankCost == 0) {
        return true;
    }
    if (award.rankCost <= 0 || beforeExperience < 0 || award.experience <= 0) {
        return false;
    }
    const auto afterExperience = static_cast<std::int64_t>(beforeExperience) + award.experience;
    const auto count = afterExperience / award.rankCost - beforeExperience / award.rankCost;
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
        || (award.rankCost > 0
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
 * Reconstructs one gear claim without publishing inventory or consuming its credit yet.
 * @param vendorIndex Installed vendor selector.
 * @param interactionIndex Rowless interaction selector.
 * @param replyIndex Reply within that interaction.
 * @param random Server-generated selection value; never supplied by the client.
 * @param mutation Receives the prepared grant; cleared on refusal.
 * @return Recognized but unsupported or unaffordable claims are refused, never generic grants.
 */
VendorReputationDisposition prepare_vendor_reward(std::uint16_t vendorIndex,
                                                  std::uint16_t interactionIndex,
                                                  std::uint16_t replyIndex,
                                                  std::uint32_t random,
                                                  PendingItemAcquisition& mutation) noexcept {
    mutation = {};
    build_data::vendors::IndexEntry entry{};
    if (!build_data::vendors::find_index(vendorIndex, entry)) {
        return VendorReputationDisposition::notApplicable;
    }
    const auto rule =
        std::find_if(kRewardRules.begin(), kRewardRules.end(), [&](const auto& value) {
            return value.vendorHash == entry.definitionHash
                   && value.interaction == interactionIndex;
        });
    if (rule == kRewardRules.end()) {
        return VendorReputationDisposition::notApplicable;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    AccountState account{};
    std::int32_t credits = 0;
    if (replyIndex != kAcceptRewardReply || !reward_binding_current(vendorIndex)
        || !investment::store::read_account(account) || !account::valid(account)
        || !runtime::detail::valid_profile_inventory(account)
        || !investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, kVanguardRewardValueRow, credits)
        || credits <= 0) {
        return VendorReputationDisposition::refused;
    }
    const auto selected = runtime::detail::selected_character_index(account);
    if (selected >= account.characterCount) {
        return VendorReputationDisposition::refused;
    }
    const auto armour = vendor_rewards::armour(account.characters[selected].characterClass);
    std::array<std::uint32_t,
               vendor_rewards::kVanguardWeapons.size() + vendor_rewards::kVanguardTitan.size()>
        candidates{};
    std::size_t count = 0;
    for (const auto pool :
         {std::span<const std::uint32_t>(vendor_rewards::kVanguardWeapons), armour}) {
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
    mutation.vendorReward = {credits, vendorIndex};
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
    if (claim.beforeCredits < 0 || !mutation.directGrant
        || !reward_binding_current(claim.vendorIndex)
        || !investment::store::read_unlock(
            investment::store::Bank::characterObjectValues, kVanguardRewardValueRow, current)
        || current != claim.beforeCredits
        || !reward_item_supported(mutation.acquiredDefinitionHash)) {
        return false;
    }
    const auto armour = vendor_rewards::armour(mutation.beforeCharacter.characterClass);
    const auto& weapons = vendor_rewards::kVanguardWeapons;
    return std::find(weapons.begin(), weapons.end(), mutation.acquiredDefinitionHash)
               != weapons.end()
           || std::find(armour.begin(), armour.end(), mutation.acquiredDefinitionHash)
                  != armour.end();
}

} // namespace sunrise::state
