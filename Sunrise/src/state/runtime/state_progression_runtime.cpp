/** Seasonal XP, Season pass claims and seasonal artifact ownership, all held in State. */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include "../build_data/runtime.h"
#include "../investment/investment.h"
#include "../investment/store_internal.h"
#include "../progression/season_pass_reward_catalog.h"
#include "../unlocks/definition.h"
#include "../unlocks/unlocks_runtime.h"
#include "runtime.h"
#include "state.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace {

namespace pass = progression::season_pass;

/** Global unlock value slot the client reads as the artifact Power bonus. */
constexpr std::uint16_t kArtifactPowerBonusSlot = 602;
/** Global unlock value slot the client reads as artifact points spent. */
constexpr std::uint16_t kArtifactPointsUsedSlot = 604;
/** Global unlock value slot the client reads as artifact points earned. */
constexpr std::uint16_t kArtifactPointsEarnedSlot = 605;
/** Character object value row whose destination is the artifact points-spent slot. */
constexpr std::uint16_t kArtifactPointsUsedCharacterRow = 38;

/** One rank of the Season of Arrivals pass costs this much XP. */
constexpr std::int32_t kExperiencePerRank = 100'000;
/** The pass stops at rank 100; XP past it feeds the repeating HUD bar instead. */
constexpr std::uint16_t kMaximumRank = 100;
constexpr std::int32_t kMaximumPassExperience =
    (static_cast<std::int32_t>(kMaximumRank) - 1) * kExperiencePerRank;

/**
 * Artifact points a mod column needs before its rows unlock, by column.
 * TODO: extract; no package field carries the column requirement.
 */
constexpr std::array<std::uint16_t, 5> kArtifactColumnTiers{0, 1, 4, 7, 10};
/** Sale rows one artifact mod column offers. */
constexpr std::uint16_t kArtifactColumnRows = 5;
/** Vendor rows before the first mod column, which are the reset row and its neighbours. */
constexpr std::uint16_t kArtifactFirstColumnRow = 6;

using SaleRows = std::array<build_data::ArtifactSaleRow, build_data::kArtifactSaleRowCapacity>;

[[nodiscard]] std::uint16_t column_tier(std::uint16_t saleIndex) noexcept {
    if (saleIndex < kArtifactFirstColumnRow) {
        return kArtifactColumnTiers[0];
    }
    const std::uint16_t column =
        static_cast<std::uint16_t>((saleIndex - kArtifactFirstColumnRow) / kArtifactColumnRows);
    return column < kArtifactColumnTiers.size() ? kArtifactColumnTiers[column]
                                                : kArtifactColumnTiers.back();
}

/** Walks one progression ladder and counts the ranks the given XP pays for. */
[[nodiscard]] std::uint16_t ladder_ranks(std::uint16_t definitionIndex,
                                         std::int32_t experience) noexcept {
    std::array<build_data::progressions::Step, build_data::progressions::kStepPerDefinitionCapacity>
        steps{};
    std::size_t stepCount = 0;
    if (!build_data::find_progression_steps(definitionIndex, steps, stepCount)) {
        return 0;
    }
    std::int64_t remaining = (std::max)(experience, 0);
    std::uint16_t ranks = 0;
    for (std::size_t step = 0; step < stepCount; ++step) {
        if (remaining < steps[step].cost) {
            break;
        }
        remaining -= steps[step].cost;
        ++ranks;
    }
    return ranks;
}

/** The ladder's first step costs nothing, so a zero total still pays for bonus 1. */
[[nodiscard]] std::uint16_t artifact_power_bonus_for(std::int32_t experience) noexcept {
    return ladder_ranks(kArtifactPowerProgressionIndex, experience);
}

[[nodiscard]] std::uint16_t artifact_points_earned_for(std::int32_t experience) noexcept {
    return ladder_ranks(kArtifactUnlockProgressionIndex, experience);
}

/**
 * Sets one global unlock value override, replacing any existing entry for the slot.
 * @return False when the slot is new and the override list is full.
 */
[[nodiscard]] bool
upsert_value(Family5State& family, std::uint16_t slot, std::int32_t value) noexcept {
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot == slot) {
            family.values[index].value = value;
            return true;
        }
    }
    if (family.valueCount >= family.values.size()) {
        return false;
    }
    family.values[family.valueCount++] = UnlockValueOverride{slot, value};
    return true;
}

[[nodiscard]] bool family5_flag_set(const Family5State& family, std::uint16_t slot) noexcept {
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        if (family.flags[index].slot == slot) {
            return family.flags[index].value == unlocks::kFlagSet;
        }
    }
    return false;
}

[[nodiscard]] bool sale_rows(SaleRows& rows, std::size_t& count) noexcept {
    count = 0;
    return build_data::artifact_sale_rows(rows, count) && count != 0;
}

/** @return One bit per sale row an authored family-5 override marks owned. Read at seed only. */
[[nodiscard]] std::uint32_t authored_artifact_mask_locked(const Family5State& family,
                                                          const SaleRows& rows,
                                                          std::size_t count) noexcept {
    std::uint32_t mask = 0;
    for (std::size_t row = 0; row < count && row < 32; ++row) {
        if (rows[row].unlockFlagSlot != build_data::collectibles::kUnavailableFlagSlot
            && family5_flag_set(family, rows[row].unlockFlagSlot)) {
            mask |= 1U << row;
        }
    }
    return mask;
}

/** @return One bit per owned artifact sale row, read from the character acquired-flag bank. */
[[nodiscard]] std::uint32_t artifact_mask(const SaleRows& rows, std::size_t count) noexcept {
    std::uint32_t mask = 0;
    for (std::size_t row = 0; row < count && row < 32; ++row) {
        const std::uint16_t mapped = rows[row].characterFlagIndex;
        if (mapped != build_data::collectibles::kUnavailableFlagIndex
            && unlocks::character_object_flag_set(mapped)) {
            mask |= 1U << row;
        }
    }
    return mask;
}

/**
 * Removes the family-5 flag rows that name artifact sale slots.
 * The character bank carries ownership, and a family-5 copy would mask it and cost 25 rows.
 * @param family Global override object, mutated in place.
 * @param rows Artifact sale rows.
 * @param count Rows in use.
 */
void strip_artifact_flags_locked(Family5State& family,
                                 const SaleRows& rows,
                                 std::size_t count) noexcept {
    std::size_t write = 0;
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        const UnlockFlagOverride flag = family.flags[index];
        bool artifact = false;
        for (std::size_t row = 0; row < count && row < 32 && !artifact; ++row) {
            artifact = rows[row].unlockFlagSlot != build_data::collectibles::kUnavailableFlagSlot
                       && rows[row].unlockFlagSlot == flag.slot;
        }
        if (!artifact) {
            family.flags[write++] = flag;
        }
    }
    for (std::size_t index = write; index < family.flagCount; ++index) {
        family.flags[index] = {};
    }
    family.flagCount = write;
}

[[nodiscard]] std::uint16_t points_used(std::uint32_t mask) noexcept {
    std::uint16_t used = 0;
    for (std::uint16_t bit = 0; bit < 32; ++bit) {
        used = static_cast<std::uint16_t>(used + ((mask & (1U << bit)) != 0 ? 1 : 0));
    }
    return used;
}

/** Character-bank write of an artifact publish; the family-5 overrides carry only the counters. */
struct CharacterArtifactWrite {
    const SaleRows* rows{};
    std::size_t count{};
    std::uint32_t mask{};
    std::uint16_t used{};
};

/**
 * Writes the artifact ownership bits and the points-used counter into the character banks.
 * @param write Mapped sale rows with the mask and the used count to publish.
 */
bool publish_artifact_character_banks(CharacterArtifactWrite& write) noexcept {
    return unlocks::mutate(&write, [](void* context, unlocks::Table& table) noexcept {
        const auto& request = *static_cast<const CharacterArtifactWrite*>(context);
        for (std::size_t row = 0; row < request.count && row < 32; ++row) {
            const std::uint16_t mapped = (*request.rows)[row].characterFlagIndex;
            if (mapped == build_data::collectibles::kUnavailableFlagIndex
                || mapped >= table.characterObjectFlags.size()) {
                continue;
            }
            table.characterObjectFlags[mapped] =
                (request.mask & (1U << row)) != 0 ? unlocks::kFlagSet : unlocks::kFlagClear;
        }
        if (kArtifactPointsUsedCharacterRow < table.characterObjectValues.size()) {
            table.characterObjectValues[kArtifactPointsUsedCharacterRow] = request.used;
        }
    });
}

/**
 * Writes one artifact ownership mask into the character bank and its counters into family 5.
 * @param family Global override object, mutated in place.
 * @param mask One bit per owned sale row.
 * @param experience Seasonal XP the derived counters are computed from.
 * @return False only when the bounded value override list is full.
 */
[[nodiscard]] bool publish_artifact_locked(Family5State& family,
                                           std::uint32_t mask,
                                           std::int32_t experience) noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return false;
    }
    strip_artifact_flags_locked(family, rows, count);
    const std::uint16_t used = points_used(mask);
    // Owned mods keep the points they cost, so XP below them (after a reset) never goes negative.
    const std::uint16_t earned = (std::max)(artifact_points_earned_for(experience), used);
    if (!upsert_value(family, kArtifactPowerBonusSlot, artifact_power_bonus_for(experience))
        || !upsert_value(family, kArtifactPointsUsedSlot, used)
        || !upsert_value(family, kArtifactPointsEarnedSlot, earned)) {
        return false;
    }
    CharacterArtifactWrite write{&rows, count, mask, used};
    return publish_artifact_character_banks(write);
}

/** Publishes the four seasonal progression lanes the current XP total implies. */
bool publish_experience_lanes(std::int32_t experience) noexcept {
    return unlocks::set_account_progression(kArtifactPowerProgressionIndex, experience)
           && unlocks::set_account_progression(kArtifactUnlockProgressionIndex, experience)
           && unlocks::set_account_progression(pass::kProgressionDefinitionIndex,
                                               (std::min)(experience, kMaximumPassExperience))
           // The HUD bar repeats past rank 100 only; earlier XP there shows a rank-100 banner.
           && unlocks::set_account_progression(
               pass::kHudProgressionDefinitionIndex,
               experience < kMaximumPassExperience ? 0 : experience - kMaximumPassExperience);
}

} // namespace

/** @return Seasonal XP published in the account progression bank. */
std::int32_t seasonal_experience() noexcept {
    return unlocks::account_progression(kArtifactPowerProgressionIndex);
}

/** Publishes every seasonal value the seeded XP and artifact ownership imply. */
bool seed_seasonal_progression() noexcept {
    const std::int32_t experience = seasonal_experience();
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return false;
    }
    investment::store::g_mutex.lock();
    investment::store::Transaction transaction;
    Family5State family;
    if (!transaction.ready() || !investment::store::read_family5(family)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    // An authored family-5 row still seeds ownership. The publish moves it to the character bank.
    const std::uint32_t authoredMask = authored_artifact_mask_locked(family, rows, count);
    if (authoredMask != 0) {
        const AccountState account = investment::store::account();
        for (std::size_t character = 0; character < account.characterCount; ++character) {
            unlocks::Table banks;
            if (!investment::store::read_unlocks(banks, static_cast<int>(character))) {
                investment::store::g_mutex.unlock();
                return false;
            }
            for (std::size_t row = 0; row < count && row < 32; ++row) {
                const auto flag = rows[row].characterFlagIndex;
                if ((authoredMask & (1U << row)) != 0 && flag < banks.characterObjectFlags.size()) {
                    banks.characterObjectFlags[flag] = unlocks::kFlagSet;
                }
            }
            if (!investment::store::write_unlocks(banks, static_cast<int>(character))) {
                investment::store::g_mutex.unlock();
                return false;
            }
        }
    }
    const std::uint32_t mask = artifact_mask(rows, count);
    const bool published = publish_experience_lanes(experience)
                           && publish_artifact_locked(family, mask, experience)
                           && investment::store::write_family5(family) && transaction.commit();
    investment::store::g_mutex.unlock();
    return published;
}

/** @return One-based Season of Arrivals rank the published XP earns. */
std::uint16_t seasonal_rank() noexcept {
    const std::int32_t earned = seasonal_experience() / kExperiencePerRank;
    return static_cast<std::uint16_t>(
        (std::min)(static_cast<std::int32_t>(kMaximumRank), earned + 1));
}

/** @return Account-wide Power bonus published by the seasonal artifact. */
std::uint16_t artifact_power_bonus() noexcept {
    investment::store::g_mutex.lock();
    Family5State family;
    (void)investment::store::read_family5(family);
    std::int32_t bonus = 0;
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot == kArtifactPowerBonusSlot) {
            bonus = family.values[index].value;
            break;
        }
    }
    investment::store::g_mutex.unlock();
    return bonus > 0 ? static_cast<std::uint16_t>(bonus) : 0U;
}

/** Adds base XP to the seasonal lanes and republishes every value derived from the total. */
bool grant_seasonal_experience(std::int32_t amount) noexcept {
    if (amount <= 0) {
        return false;
    }
    investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    const std::int32_t previous = seasonal_experience();
    if (previous > (std::numeric_limits<std::int32_t>::max)() - amount) {
        return false;
    }
    const std::int32_t total = previous + amount;

    Family5State family;
    if (!transaction.ready() || !investment::store::read_family5(family)) {
        return false;
    }
    SaleRows rows{};
    std::size_t count = 0;
    const std::uint32_t mask = sale_rows(rows, count) ? artifact_mask(rows, count) : 0U;
    const bool saved = publish_experience_lanes(total)
                       && publish_artifact_locked(family, mask, total)
                       && investment::store::write_family5(family) && transaction.commit();
    return saved;
}

/** Publishes zero XP the way a grant publishes its total, keeping the owned artifact mods. */
bool reset_seasonal_experience() noexcept {
    investment::store::Transaction transaction;
    SaleRows rows{};
    std::size_t count = 0;
    Family5State family;
    if (!transaction.ready() || !sale_rows(rows, count)
        || !investment::store::read_family5(family)) {
        return false;
    }
    const std::uint32_t mask = artifact_mask(rows, count);
    return publish_experience_lanes(0) && publish_artifact_locked(family, mask, 0)
           && investment::store::write_family5(family) && transaction.commit();
}

/** The pass threshold is the only cap; below it the whole amount fits. */
std::int32_t seasonal_experience_within_pass(std::int32_t amount) noexcept {
    // A negative total is invalid State; count from zero instead of below it.
    const std::int32_t room = kMaximumPassExperience - (std::max)(seasonal_experience(), 0);
    return amount > 0 && room > 0 ? (std::min)(amount, room) : 0;
}

/** Whole ranks keep the progress inside the current rank until the pass threshold cuts it. */
std::int32_t seasonal_experience_to_advance(std::uint16_t ranks) noexcept {
    // No request needs more than the whole pass, which keeps the product inside the XP type.
    const std::int64_t wanted = (std::min)(static_cast<std::int64_t>(ranks) * kExperiencePerRank,
                                           static_cast<std::int64_t>(kMaximumPassExperience));
    return seasonal_experience_within_pass(static_cast<std::int32_t>(wanted));
}

/** @return True when this Season pass reward row is already claimed. */
bool season_pass_reward_claimed(std::uint16_t rewardIndex) noexcept {
    build_data::season_pass::Reward reward{};
    return build_data::find_season_pass_reward(rewardIndex, reward)
           && reward.claimFlagIndex != build_data::season_pass::kUnavailableFlagIndex
           && unlocks::account_flag_set(reward.claimFlagIndex);
}

/** Claims one Season pass reward row into the account flag its row names. */
bool claim_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    build_data::season_pass::Reward reward{};
    if (!build_data::find_season_pass_reward(rewardIndex, reward)
        || reward.claimFlagIndex == build_data::season_pass::kUnavailableFlagIndex
        || unlocks::account_flag_set(reward.claimFlagIndex)) {
        return false;
    }
    return unlocks::set_account_flag(reward.claimFlagIndex, unlocks::kFlagSet);
}

/** Undoes one Season pass claim so a refused commit cannot leave it held. */
void revoke_season_pass_reward(std::uint16_t rewardIndex) noexcept {
    build_data::season_pass::Reward reward{};
    if (build_data::find_season_pass_reward(rewardIndex, reward)
        && reward.claimFlagIndex != build_data::season_pass::kUnavailableFlagIndex) {
        (void)unlocks::set_account_flag(reward.claimFlagIndex, unlocks::kFlagClear);
    }
}

/** @return Purchased artifact sale rows, one bit per row. */
std::uint32_t artifact_mod_mask() noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return 0;
    }
    return artifact_mask(rows, count);
}

/** Replaces the exact published mask, refusing when another action changed it first. */
bool replace_artifact_mod_mask(std::uint32_t expected, std::uint32_t replacement) noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return false;
    }
    const std::int32_t experience = seasonal_experience();
    investment::store::g_mutex.lock();
    investment::store::Transaction transaction;
    Family5State family;
    if (!transaction.ready() || !investment::store::read_family5(family)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    bool replaced = artifact_mask(rows, count) == expected;
    if (replaced) {
        replaced = publish_artifact_locked(family, replacement, experience)
                   && investment::store::write_family5(family) && transaction.commit();
    }
    investment::store::g_mutex.unlock();
    return replaced;
}

/** Writes one affordable artifact purchase and keeps its before-image for the commit. */
bool prepare_artifact_mod_unlock(std::uint16_t saleIndex,
                                 PendingArtifactPurchase& mutation) noexcept {
    mutation = {};
    SaleRows rows{};
    std::size_t count = 0;
    const AccountState account = account_snapshot();
    if (!sale_rows(rows, count) || saleIndex >= count || saleIndex >= 32
        || rows[saleIndex].unlockFlagSlot == build_data::collectibles::kUnavailableFlagSlot
        || !account::valid(account)) {
        return false;
    }
    std::size_t selected = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            selected = index;
            break;
        }
    }
    if (selected >= account.characterCount) {
        return false;
    }
    const std::uint32_t bit = 1U << saleIndex;
    const std::int32_t experience = seasonal_experience();
    const std::uint16_t earned = artifact_points_earned_for(experience);
    investment::store::g_mutex.lock();
    investment::store::Transaction transaction;
    Family5State family;
    if (!transaction.ready() || !investment::store::read_family5(family)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    const std::uint32_t before = artifact_mask(rows, count);
    const std::uint16_t used = points_used(before);
    bool prepared = (before & bit) == 0 && used < earned && used >= column_tier(saleIndex);
    if (prepared) {
        prepared = publish_artifact_locked(family, before | bit, experience)
                   && investment::store::write_family5(family) && transaction.commit();
    }
    investment::store::g_mutex.unlock();
    if (!prepared) {
        return false;
    }
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = account.characters[selected].soid;
    mutation.characterIndex = selected;
    mutation.beforeMask = before;
    mutation.afterMask = before | bit;
    mutation.saleIndex = saleIndex;
    mutation.prepared = true;
    return true;
}

/** Keeps a prepared purchase only while its character and mask are still the prepared ones. */
bool commit_artifact_mod_unlock(PendingArtifactPurchase& mutation) noexcept {
    const PendingArtifactPurchase prepared = mutation;
    mutation = {};
    const AccountState account = account_snapshot();
    return prepared.prepared && prepared.accountSoid != 0 && prepared.characterSoid != 0
           && prepared.beforeMask != prepared.afterMask
           && prepared.characterIndex < account.characterCount
           && account.primarySoid == prepared.accountSoid
           && account.characters[prepared.characterIndex].soid == prepared.characterSoid
           && account.characters[prepared.characterIndex].selected
           && artifact_mod_mask() == prepared.afterMask;
}

} // namespace sunrise::state
