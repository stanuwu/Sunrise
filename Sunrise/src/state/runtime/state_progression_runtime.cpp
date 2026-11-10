/** Seasonal XP, Season pass claims and seasonal artifact ownership, all held in State. */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>

#include "../../core/logging/log.h"
#include "../build_data/runtime.h"
#include "../investment/investment.h"
#include "../investment/store_internal.h"
#include "../progression/season_pass_reward_catalog.h"
#include "../rewards/reward_resolver.h"
#include "../unlocks/definition.h"
#include "../unlocks/unlocks_runtime.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
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
/** The pass stops at rank 100; XP past it feeds the repeating progression instead. */
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

/** An ownership mask holds one bit per artifact sale row. */
constexpr std::size_t kArtifactMaskRows = std::numeric_limits<std::uint32_t>::digits;
// An unavailable flag index lies past the character bank, so the bounds check skips it.
static_assert(unlocks::kCharacterObjectFlagCapacity
              <= build_data::collectibles::kUnavailableFlagIndex);

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

/** @return One-based Season of Arrivals rank the given XP total earns. */
[[nodiscard]] std::uint16_t seasonal_rank_for(std::int32_t experience) noexcept {
    const std::int32_t earned = experience / kExperiencePerRank;
    return static_cast<std::uint16_t>(
        (std::min)(static_cast<std::int32_t>(kMaximumRank), earned + 1));
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
    for (std::size_t row = 0; row < count && row < kArtifactMaskRows; ++row) {
        if (rows[row].unlockFlagSlot != build_data::collectibles::kUnavailableFlagSlot
            && family5_flag_set(family, rows[row].unlockFlagSlot)) {
            mask |= 1U << row;
        }
    }
    return mask;
}

/** @return One bit per artifact sale row the character's acquired-flag bank marks owned. */
[[nodiscard]] std::uint32_t character_bank_artifact_mask(const unlocks::Table& banks,
                                                         const SaleRows& rows,
                                                         std::size_t count) noexcept {
    std::uint32_t mask = 0;
    for (std::size_t row = 0; row < count && row < kArtifactMaskRows; ++row) {
        const std::uint16_t flag = rows[row].characterFlagIndex;
        if (flag < banks.characterObjectFlags.size()
            && banks.characterObjectFlags[flag] == unlocks::kFlagSet) {
            mask |= 1U << row;
        }
    }
    return mask;
}

/**
 * Reads one bit per owned artifact sale row from the selected character's bank.
 * @param mask Receives the bits, or zero on failure.
 * @return False when the bank copy cannot be allocated or read.
 */
[[nodiscard]] bool
artifact_mask(const SaleRows& rows, std::size_t count, std::uint32_t& mask) noexcept {
    mask = 0;
    const std::unique_ptr<unlocks::Table> banks(new (std::nothrow) unlocks::Table{});
    if (!banks || !investment::store::read_unlocks(*banks)) {
        return false;
    }
    mask = character_bank_artifact_mask(*banks, rows, count);
    return true;
}

/**
 * Hands every character's unlock banks to visit in slot order, reusing one heap copy.
 * @param visit Takes the slot and its banks, and returns false to stop the walk.
 * @return False when the bank copy cannot be allocated, the account or a bank cannot be read, or
 * a visit returns false.
 */
template <typename Visit> [[nodiscard]] bool visit_character_banks(Visit visit) noexcept {
    AccountIdentity account{};
    const std::unique_ptr<unlocks::Table> banks(new (std::nothrow) unlocks::Table{});
    if (!banks || !investment::store::read_account_identity(account)) {
        return false;
    }
    for (std::size_t slot = 0; slot < account.characterCount; ++slot) {
        if (!investment::store::read_unlocks(*banks, static_cast<int>(slot))
            || !visit(static_cast<int>(slot), *banks)) {
            return false;
        }
    }
    return true;
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
        for (std::size_t row = 0; row < count && row < kArtifactMaskRows && !artifact; ++row) {
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
    for (std::uint16_t bit = 0; bit < kArtifactMaskRows; ++bit) {
        used = static_cast<std::uint16_t>(used + ((mask & (1U << bit)) != 0 ? 1 : 0));
    }
    return used;
}

/**
 * Reads the largest artifact points-used count across every character bank. Ownership is read
 * through the artifact sale rows, so this waits for the catalogs those rows resolve through. Rows
 * the published catalogs still cannot resolve are refused, since no retry changes them.
 * @param most Receives the largest count.
 * @param reason Receives the refusal label on failure.
 * @return False when the rows are unusable, or the account or a character bank cannot be read.
 */
[[nodiscard]] bool most_artifact_points_used_locked(std::uint16_t& most,
                                                    const char*& reason) noexcept {
    most = 0;
    reason = "not_ready";
    if (!build_data::vendor_catalog_ready() || !build_data::item_definitions_ready()
        || !build_data::collectible_definitions_ready()) {
        return false;
    }
    reason = "artifact_unsupported";
    SaleRows rows{};
    std::size_t count = 0;
    if (!sale_rows(rows, count)) {
        return false;
    }
    reason = "transaction";
    return visit_character_banks([&](int /*slot*/, const unlocks::Table& banks) noexcept {
        most = (std::max)(most, points_used(character_bank_artifact_mask(banks, rows, count)));
        return true;
    });
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
        for (std::size_t row = 0; row < request.count && row < kArtifactMaskRows; ++row) {
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
    if (!upsert_value(family, kArtifactPowerBonusSlot, artifact_power_bonus_for(experience))
        || !upsert_value(family, kArtifactPointsUsedSlot, used)
        || !upsert_value(
            family, kArtifactPointsEarnedSlot, artifact_points_earned_for(experience))) {
        return false;
    }
    CharacterArtifactWrite write{&rows, count, mask, used};
    return publish_artifact_character_banks(write);
}

/** Logs one earned perk whose flags were not granted, and why. */
void report_perk_refusal(std::size_t index, const char* reason) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=season_xp stage=automatic_perk index=%zu result=skip reason=%s",
                      index,
                      reason);
}

// The walk skips an absent acquisition flag and an absent claim flag on one sentinel.
static_assert(build_data::rewards::kAbsent == build_data::season_pass::kUnavailableFlagIndex);

/**
 * Sets the acquisition and claim flags of every perk a rank earns in one character's bank image.
 * A perk's conditions read the flags an earlier perk of the same walk set.
 * @param flags The character's banks, which receive every flag the walk sets.
 * @param characterClass Class the perk conditions read.
 * @param rank One-based season rank the perks are earned at.
 * @param save Saves one flag the walk sets, or null when the image is a preview.
 * @param earned Set when the walk set at least one flag.
 * @return False when a flag could not be saved.
 */
bool apply_earned_perks(unlocks::Table& flags,
                        CharacterClass characterClass,
                        std::uint16_t rank,
                        bool (*save)(std::uint16_t) noexcept,
                        bool& earned) noexcept {
    const auto count = build_data::season_pass_reward_count();
    for (std::size_t index = 0; index < count; ++index) {
        build_data::season_pass::Reward reward{};
        if (!build_data::find_season_pass_reward(static_cast<std::uint16_t>(index), reward)) {
            continue;
        }
        if (reward.requiredRank > rank) {
            continue;
        }
        const auto acquired = pass::progress_flag(reward);
        if (acquired == build_data::rewards::kAbsent) {
            continue;
        }
        const auto claim = reward.claimFlagIndex;
        if (acquired >= flags.accountFlags.size() || reward.quantity != 1 || reward.socketCount != 0
            || reward.conditionCount > reward.condition.size()
            || (claim != build_data::season_pass::kUnavailableFlagIndex
                && claim >= flags.accountFlags.size())) {
            report_perk_refusal(index, "perk_shape");
            continue;
        }
        const char* reason = nullptr;
        bool eligible = false;
        if (!reward_resolution::eligible(std::span(reward.condition).first(reward.conditionCount),
                                         {flags, characterClass, 0, &reason},
                                         eligible)) {
            report_perk_refusal(index, reason != nullptr ? reason : "perk_condition");
            continue;
        }
        if (!eligible) {
            continue;
        }
        for (const auto flag : {acquired, claim}) {
            if (flag == build_data::rewards::kAbsent
                || flags.accountFlags[flag] == unlocks::kFlagSet) {
                continue;
            }
            if (save != nullptr && !save(flag)) {
                return false;
            }
            flags.accountFlags[flag] = unlocks::kFlagSet;
            earned = true;
        }
    }
    return true;
}

/**
 * Sets the acquisition and claim flags of every perk the selected character has earned.
 * It sets only clear flags, so it runs on every rank change, pick, publication and artifact change.
 * @param earned Set when the walk set at least one flag.
 * @return False when the save could not be read or written.
 */
bool walk_earned_perks(bool& earned) noexcept {
    // Unpublished rows earn nothing yet; their publication walks again.
    if (!season_perks_walkable()) {
        return true;
    }
    AccountState account;
    if (!investment::store::read_account(account)) {
        return false;
    }
    const auto character = runtime::detail::selected_character_index(account);
    if (character >= account.characterCount) {
        return true;
    }
    unlocks::Table flags{};
    if (!investment::store::read_unlocks(flags, static_cast<int>(character))) {
        return false;
    }
    return apply_earned_perks(flags,
                              account.characters[character].characterClass,
                              seasonal_rank(),
                              [](std::uint16_t flag) noexcept {
                                  return unlocks::set_account_flag(flag, unlocks::kFlagSet);
                              },
                              earned);
}

/** One seasonal progression and the value an XP total gives its first lane. */
struct ExperienceLane {
    std::uint16_t definitionIndex{};
    std::int32_t value{};
};

// A preview writes these rows into a bank image without the bounds check a save makes.
static_assert((std::max)({kArtifactPowerProgressionIndex,
                          kArtifactUnlockProgressionIndex,
                          pass::kProgressionDefinitionIndex,
                          pass::kHudProgressionDefinitionIndex})
              < build_data::progressions::kDefinitionCapacity);

/** @return The seasonal progressions with the values the given XP total gives them. */
[[nodiscard]] auto experience_lanes(std::int32_t experience) noexcept {
    return std::array{ExperienceLane{kArtifactPowerProgressionIndex, experience},
                      ExperienceLane{kArtifactUnlockProgressionIndex, experience},
                      ExperienceLane{pass::kProgressionDefinitionIndex,
                                     (std::min)(experience, kMaximumPassExperience)},
                      ExperienceLane{pass::kHudProgressionDefinitionIndex,
                                     (std::max)(experience - kMaximumPassExperience, 0)}};
}

/** Publishes the four seasonal progression lanes the current XP total implies. */
bool publish_experience_lanes(std::int32_t experience) noexcept {
    const auto lanes = experience_lanes(experience);
    return std::all_of(lanes.begin(), lanes.end(), [](const ExperienceLane& lane) noexcept {
        return unlocks::set_account_progression(lane.definitionIndex, lane.value);
    });
}

/**
 * Publishes a new XP total with its artifact values and commits the caller's transaction.
 * @param rankReached True when the total reaches a higher rank, the only XP change that can earn
 *        a perk.
 */
[[nodiscard]] bool publish_experience_total(investment::store::Transaction& transaction,
                                            std::int32_t total,
                                            bool rankReached) noexcept {
    Family5State family;
    if (!investment::store::read_family5(family)) {
        return false;
    }
    SaleRows rows{};
    std::size_t count = 0;
    std::uint32_t mask = 0;
    // Publishing a mask that could not be read would clear every owned row.
    if (sale_rows(rows, count) && !artifact_mask(rows, count, mask)) {
        return false;
    }
    // The caller publishes the whole account image, so it needs no report.
    bool earnedRidesTheImage = false;
    return publish_experience_lanes(total) && publish_artifact_locked(family, mask, total)
           && investment::store::write_family5(family)
           && (!rankReached || walk_earned_perks(earnedRidesTheImage)) && transaction.commit();
}

/**
 * Finds a native pass row that still grants the expected item and that the player claims. A row
 * the earned-perk walk grants is refused, because that walk owns its claim flag.
 */
[[nodiscard]] bool find_claimable_reward(std::uint16_t rewardIndex,
                                         std::uint32_t itemHash,
                                         build_data::season_pass::Reward& reward,
                                         const char*& reason) noexcept {
    // The automatic-row check reads the item and reward catalogs.
    reason = "not_ready";
    if (!season_perks_walkable()) {
        return false;
    }
    reason = "reward_index";
    if (!build_data::find_season_pass_reward(rewardIndex, reward)) {
        return false;
    }
    reason = "reward_hash";
    if (reward.itemHash != itemHash) {
        return false;
    }
    reason = "automatic_reward";
    return !pass::automatic_row(reward);
}

} // namespace

/** Packages share the perk bucket; only rewards without a pool grant the flag directly. */
std::uint16_t
progression::season_pass::progress_flag(const build_data::season_pass::Reward& reward) noexcept {
    build_data::items::Definition item{};
    build_data::rewards::Item acquisition{};
    if (build_data::find_item_definition_index(reward.itemIndex, item)
        && item.definitionHash == reward.itemHash
        && item.bucketId == build_data::inventory::buckets::kPerkBucketId
        && build_data::find_reward_item(reward.itemIndex, acquisition)
        && acquisition.definitionHash == reward.itemHash
        && acquisition.poolIndex == build_data::rewards::kAbsent) {
        return acquisition.acquiredFlag;
    }
    return build_data::rewards::kAbsent;
}

/** A row with a direct perk flag is granted by the earned-perk walk, never claimed. */
bool progression::season_pass::automatic_row(
    const build_data::season_pass::Reward& reward) noexcept {
    return progress_flag(reward) != build_data::rewards::kAbsent;
}

/** @return Seasonal XP published in the account progression bank. */
std::int32_t seasonal_experience() noexcept {
    return unlocks::account_progression(kArtifactPowerProgressionIndex);
}

/** Publishes every seasonal value the seeded XP and artifact ownership imply. */
bool seed_seasonal_progression(bool& perksEarned) noexcept {
    perksEarned = false;
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
    const auto seedOwnership = [&](int slot, unlocks::Table& banks) noexcept {
        for (std::size_t row = 0; row < count && row < kArtifactMaskRows; ++row) {
            const auto flag = rows[row].characterFlagIndex;
            if ((authoredMask & (1U << row)) != 0 && flag < banks.characterObjectFlags.size()) {
                banks.characterObjectFlags[flag] = unlocks::kFlagSet;
            }
        }
        return investment::store::write_unlocks(banks, slot);
    };
    if (authoredMask != 0 && !visit_character_banks(seedOwnership)) {
        investment::store::g_mutex.unlock();
        return false;
    }
    std::uint32_t mask = 0;
    // A pick changes which class and character flags the perk conditions read.
    bool earned = false;
    const bool published = artifact_mask(rows, count, mask) && publish_experience_lanes(experience)
                           && publish_artifact_locked(family, mask, experience)
                           && investment::store::write_family5(family) && walk_earned_perks(earned)
                           && transaction.commit();
    perksEarned = published && earned;
    investment::store::g_mutex.unlock();
    return published;
}

bool season_perks_walkable() noexcept {
    return build_data::season_pass_ready() && build_data::reward_definitions_ready()
           && build_data::item_definitions_ready();
}

/** Walks the selected character's earned perks on its own, outside a pick or a rank change. */
bool grant_earned_season_perks(bool& perksEarned) noexcept {
    perksEarned = false;
    investment::store::Transaction transaction;
    bool earned = false;
    const bool walked = transaction.ready() && walk_earned_perks(earned) && transaction.commit();
    perksEarned = walked && earned;
    return walked;
}

/** @return One-based Season of Arrivals rank the published XP earns. */
std::uint16_t seasonal_rank() noexcept {
    return seasonal_rank_for(seasonal_experience());
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

/** Publishes seasonal XP, artifact progress and newly earned acquisition flags atomically. */
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
    const auto total = previous + amount;
    return publish_experience_total(
        transaction, total, seasonal_rank_for(previous) != seasonal_rank_for(total));
}

namespace runtime::detail {

/** Applies the lanes and earned-perk flags grant_seasonal_experience saves for the same amount. */
bool preview_seasonal_experience(std::int32_t expected,
                                 std::int32_t amount,
                                 unlocks::Table& after) noexcept {
    const std::int32_t previous = seasonal_experience();
    if (amount <= 0 || previous != expected
        || previous > (std::numeric_limits<std::int32_t>::max)() - amount) {
        return false;
    }
    const std::int32_t total = previous + amount;
    for (const ExperienceLane& lane : experience_lanes(total)) {
        after.accountProgressions[lane.definitionIndex].front() = lane.value;
    }
    // Only a new rank earns a perk, and unpublished rows earn nothing yet.
    if (seasonal_rank_for(previous) == seasonal_rank_for(total) || !season_perks_walkable()) {
        return true;
    }
    AccountState account;
    if (!investment::store::read_account(account)) {
        return false;
    }
    const auto character = selected_character_index(account);
    // The frame built from this image carries the flags, so the walk needs no report.
    bool earnedRidesTheImage = false;
    return character >= account.characterCount
           || apply_earned_perks(after,
                                 account.characters[character].characterClass,
                                 seasonal_rank_for(total),
                                 nullptr,
                                 earnedRidesTheImage);
}

} // namespace runtime::detail

/** Refuses a decrease below the artifact points any character has spent. */
bool replace_seasonal_experience(std::int32_t expected,
                                 std::int32_t replacement,
                                 const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    if (!check_seasonal_experience(replacement, &reason)) {
        return false;
    }
    reason = "transaction";
    investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    reason = "experience_changed";
    const std::int32_t previous = seasonal_experience();
    if (previous != expected) {
        return false;
    }
    // Only a decrease can fall below the artifact points a character has spent.
    std::uint16_t spent = 0;
    if (replacement < previous && !most_artifact_points_used_locked(spent, reason)) {
        return false;
    }
    // Selecting the character that spent the most would otherwise publish more points spent than
    // the new total earns.
    reason = "experience_spent";
    if (spent > artifact_points_earned_for(replacement)) {
        return false;
    }
    reason = "transaction";
    const bool rankReached =
        replacement > previous && seasonal_rank_for(previous) != seasonal_rank_for(replacement);
    if (!publish_experience_total(transaction, replacement, rankReached)) {
        return false;
    }
    reason = nullptr;
    return true;
}

/** An XP total counts earned XP, so it is never negative. */
bool check_seasonal_experience(std::int32_t replacement, const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "experience_range";
    if (replacement < 0) {
        return false;
    }
    reason = nullptr;
    return true;
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

/** Clears one claimed row's claim flag while the row still names the expected item. */
bool reset_season_pass_reward(std::uint16_t rewardIndex,
                              std::uint32_t itemHash,
                              const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    build_data::season_pass::Reward reward{};
    if (!find_claimable_reward(rewardIndex, itemHash, reward, reason)) {
        return false;
    }
    reason = "transaction";
    investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    reason = "not_claimed";
    if (!season_pass_reward_claimed(rewardIndex)) {
        return false;
    }
    revoke_season_pass_reward(rewardIndex);
    reason = "transaction";
    if (season_pass_reward_claimed(rewardIndex) || !transaction.commit()) {
        return false;
    }
    reason = nullptr;
    return true;
}

/** Reads only installed catalogs. */
bool check_season_pass_reward(std::uint16_t rewardIndex,
                              std::uint32_t itemHash,
                              const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    build_data::season_pass::Reward reward{};
    if (!find_claimable_reward(rewardIndex, itemHash, reward, reason)) {
        return false;
    }
    reason = nullptr;
    return true;
}

/** @return Purchased artifact sale rows, one bit per row; zero when they cannot be read. */
std::uint32_t artifact_mod_mask() noexcept {
    SaleRows rows{};
    std::size_t count = 0;
    std::uint32_t mask = 0;
    const bool read = sale_rows(rows, count) && artifact_mask(rows, count, mask);
    return read ? mask : 0U;
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
    std::uint32_t published = 0;
    bool replaced = artifact_mask(rows, count, published) && published == expected;
    // A reset publishes the account image after this; a refused purchase rolls the walk back.
    bool earnedRidesTheImage = false;
    if (replaced) {
        replaced = publish_artifact_locked(family, replacement, experience)
                   && investment::store::write_family5(family)
                   && walk_earned_perks(earnedRidesTheImage) && transaction.commit();
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
    const AccountState account = bound_account_snapshot();
    if (!sale_rows(rows, count) || saleIndex >= count || saleIndex >= kArtifactMaskRows
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
    std::uint32_t before = 0;
    const bool read = artifact_mask(rows, count, before);
    const std::uint16_t used = points_used(before);
    bool prepared = read && (before & bit) == 0 && used < earned && used >= column_tier(saleIndex);
    bool perksEarned = false;
    if (prepared) {
        prepared = publish_artifact_locked(family, before | bit, experience)
                   && investment::store::write_family5(family) && walk_earned_perks(perksEarned)
                   && transaction.commit();
    }
    investment::store::g_mutex.unlock();
    if (!prepared) {
        return false;
    }
    mutation.perksEarned = perksEarned;
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
    const AccountState account = bound_account_snapshot();
    return prepared.prepared && prepared.accountSoid != 0 && prepared.characterSoid != 0
           && prepared.beforeMask != prepared.afterMask
           && prepared.characterIndex < account.characterCount
           && account.primarySoid == prepared.accountSoid
           && account.characters[prepared.characterIndex].soid == prepared.characterSoid
           && account.characters[prepared.characterIndex].selected
           && artifact_mod_mask() == prepared.afterMask;
}

} // namespace sunrise::state
