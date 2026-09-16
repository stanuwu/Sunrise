#include "state_vendor_reputation_runtime.h"

#include <algorithm>
#include <limits>

#include "../build_data/vendors/vendor_catalog.h"
#include "../investment/store_internal.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {

/** Build-86657 faction tokenValues apply only to the matching vendor sale placeholder. */
struct ReputationRule {
    std::uint32_t vendorHash;
    std::uint32_t placeholderHash;
    std::uint32_t costHash;
    std::uint16_t progressionIndex;
    std::int32_t experiencePerUnit;
};

/** Vendor/item hashes, character progression indices and XP per unit from build 86657. */
constexpr std::array<ReputationRule, 7> kReputationRules{{
    // Banshee: Gunsmith Rewards charges Gunsmith Materials for Gunsmith progression.
    {672118013U, 3831705402U, 685157383U, 55, 30},
    // Banshee: the same placeholder also accepts Weapon Telemetry at its own XP rate.
    {672118013U, 3831705402U, 685157381U, 55, 25},
    // Zavala: Vanguard Tactician Rewards charges Vanguard Tactician Tokens.
    {69482069U, 3987308529U, 3899548068U, 62, 100},
    // Shaxx: Crucible Rewards charges Crucible Tokens, not Valor or Glory points.
    {3603221665U, 265113466U, 183980811U, 49, 100},
    // Devrim: EDZ token turn-ins use a different placeholder from destination materials.
    {396892126U, 61430328U, 2640973641U, 52, 100},
    // Devrim: destination-material turn-ins accept Dusklight Shards.
    {396892126U, 1317670974U, 950899352U, 52, 50},
    // Devrim: the same material placeholder accepts Dusklight Crystals at their own XP rate.
    {396892126U, 1317670974U, 478751073U, 52, 250},
}};

/** Native progression level walks read experience from lane zero. */
constexpr std::size_t kExperienceLane = 0;

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
                 rule.progressionIndex};
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
    if (!charge_materials(after, award)) {
        return VendorReputationDisposition::refused;
    }
    mutation.beforeItems = account.profileItems;
    mutation.beforeItemCount = account.profileItemCount;
    mutation.beforeProgression = before;
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
 * Debits materials and credits character XP within the caller's response transaction.
 * @param mutation Before-image to consume, including on refusal.
 * @return False for stale state, changed content or any failed write; neither change is kept.
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
        || progression[kExperienceLane]
               > (std::numeric_limits<std::int32_t>::max)() - award.experience
        || !charge_materials(account, award)) {
        return false;
    }
    progression[kExperienceLane] += award.experience;
    return investment::store::write_account(account)
           && investment::store::write_unlocks(banks, static_cast<int>(mutation.characterIndex))
           && transaction.commit();
}

} // namespace sunrise::state
