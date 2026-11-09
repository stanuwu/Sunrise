/**
 * Pursuit redemption: a completed pursuit leaves inventory through the shared reward grant, which
 * also moves its quest set to the next step and places what that step pays.
 */
#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>

#include "../build_data/runtime.h"
#include "../build_data/vendors/repeatable_triggers.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
#include "pursuit_completion_runtime.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace inventory_buckets = build_data::inventory::buckets;
namespace progressions = build_data::progressions;
namespace pursuits = build_data::pursuits;
using Quest = build_data::items::QuestInitialization;

static_assert(pursuits::kItemValueCapacity == authored_inventory::kItemObjectiveLaneCount);

namespace {

/**
 * What redeeming one pursuit grants. The objectives, per-item values, reward rows and quest set
 * it is built from are read from the installed packages; the effects are reconstructed. Native
 * completion runs operation bodies the packages do not describe, so the server instead removes
 * the pursuit, moves its quest set's saved step to the next member and grants that member. Only
 * the last member, or a pursuit outside any set, grants the reward rows it shows, so a set pays
 * them once.
 */
struct CompletionPlan {
    std::array<DirectRecordReward, kRecordRewardGrantCapacity> rows{};
    std::size_t rowCount{};
    std::bitset<unlocks::kAccountFlagCapacity> wrapperFlags{};
    std::array<reward_resolution::Grant, kRecordRewardGrantCapacity> grants{};
    PreparedQuestStep questStep{};
    PreparedProgressionCredit progression{};
    std::int32_t seasonalExperience{};
};

/**
 * A reward row names a stand-in rather than an item when nothing in the packages turns it into
 * inventory: its item is homed in a bucket that keeps only its newest row, or it is a perk-bucket
 * item with no acquisition flag to set. In the shipped build every experience, gear, powerful and
 * pinnacle marker and the Glimmer and Bright Dust stand-ins sit in the build's only one-row FIFO
 * bucket, and the rank-point markers in the perk bucket. What each one pays is server policy the
 * packages do not hold: the authored tables below pay the ones they name, and any other row naming
 * a stand-in grants nothing. A Dawning ingredient pickup in the perk bucket is no stand-in: the
 * grant places it and credits the oven balance it fills.
 */
[[nodiscard]] bool names_stand_in(std::uint16_t itemIndex) noexcept {
    build_data::items::Definition item{};
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_item_definition_index(itemIndex, item)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return false;
    }
    if (bucket.slotCount == 1 && (bucket.policyFlags & inventory_buckets::kFifo) != 0) {
        return true;
    }
    build_data::rewards::Item reward{};
    return item.bucketId == inventory_buckets::kPerkBucketId
           && !is_ingredient_pickup(item.definitionHash)
           && build_data::find_reward_item(itemIndex, reward)
           && reward.acquiredFlag == build_data::rewards::kAbsent;
}

/**
 * A quest step is redeemed here only when every objective it counts is held by the item. A step
 * whose progress lives in the saved banks is advanced by whatever writes those banks.
 */
[[nodiscard]] bool progress_item_held(const PursuitCompletionStatus& status) noexcept {
    const auto objectives = std::span(status.objectives).first(status.objectiveCount);
    return !objectives.empty()
           && std::all_of(objectives.begin(), objectives.end(), [](const auto& objective) {
                  return objective.skipped || objective.itemHeld;
              });
}

/**
 * A bounty's deadline is its first per-item value, so an objective read from that value would
 * count the deadline as progress.
 */
[[nodiscard]] bool reads_deadline(const pursuits::Definition& contract) noexcept {
    if (contract.lifetimeSeconds <= 0 || contract.itemValueSlotCount == 0) {
        return false;
    }
    const std::uint16_t deadline = contract.itemValueSlots[authored_inventory::kItemExpiryLane];
    const auto objectives = std::span(contract.objectives).first(contract.objectiveCount);
    for (const std::uint16_t index : objectives) {
        build_data::objectives::Definition objective{};
        if (build_data::find_objective_definition(index, objective)
            && objective.itemValueSlot == deadline) {
            return true;
        }
    }
    return false;
}

/**
 * Plans the next quest step: the saved step must name this pursuit, and its successor is granted.
 * @return False when the saved step names another member.
 */
[[nodiscard]] bool plan_quest_step(const pursuits::QuestStep& step,
                                   unlocks::Table& banks,
                                   CompletionPlan& plan) noexcept {
    const std::int32_t saved = quest_value(banks, {step.value, step.row, step.scope});
    if (saved != step.value) {
        return false;
    }
    if (step.successorIndex != pursuits::kAbsent) {
        plan.questStep = {{step.successorValue, step.row, step.scope}, saved};
        plan.rows[plan.rowCount++] = {step.successorIndex, 1};
    }
    return true;
}

/** How often a bounty can be taken, which scales what its markers pay. */
enum class Cadence : std::uint8_t {
    /** No endless pool holds the pursuit and it never expires, so it is no bounty. */
    none,
    repeatable,
    daily,
    weekly,
};

/** A bounty that lasts a week is a weekly one. */
constexpr std::int32_t kWeekSeconds = 7 * 24 * 60 * 60;

/** Items a gear marker draws from: weapons any class draws and each class's armour. */
struct GearPool {
    std::span<const std::uint32_t> weapons;
    std::span<const std::uint32_t> titanArmour;
    std::span<const std::uint32_t> hunterArmour;
    std::span<const std::uint32_t> warlockArmour;
};

/** A gear marker and the pool it draws one item from. */
struct GearMarker {
    std::uint32_t hash{};
    GearPool pool{};
};

/** A gear marker that pays one fixed armour piece per class. */
struct ClassArmourMarker {
    std::uint32_t hash{};
    std::uint32_t titanArmour{};
    std::uint32_t hunterArmour{};
    std::uint32_t warlockArmour{};
};

/** A row naming this item pays the currency; a row with no amount pays its cadence's amount. */
struct CurrencyPayout {
    std::uint32_t hash{};
    std::uint32_t currencyHash{};
    /** Paid by a repeatable or daily bounty. */
    std::int32_t dailyQuantity{};
    std::int32_t weeklyQuantity{};
};

/** A rank-point marker and the progression its row's quantity credits. */
struct RankMarker {
    std::uint32_t hash{};
    std::uint32_t progressionHash{};
};

// --- Authored game data --------------------------------------------------------------------

// TODO: extract what these markers pay; no extractor relates a reward marker to the experience,
// currency, progression or loot the retail server paid for it.

/** Seasonal XP an experience marker pays, by cadence. */
constexpr std::int32_t kRepeatableBountyExperience = 4'000;
constexpr std::int32_t kDailyBountyExperience = 6'000;
constexpr std::int32_t kWeeklyBountyExperience = 12'000;

/** The double experience marker pays twice its cadence's XP. */
constexpr std::uint32_t kExperienceMarkerHash = 2211488305U;
constexpr std::uint32_t kDoubleExperienceMarkerHash = 583797518U;

constexpr std::array<CurrencyPayout, 3> kCurrencyPayouts{{
    {3085039018U, 3159615086U, 1'000, 3'000}, // Glimmer stand-in
    {3159615086U, 3159615086U, 1'000, 3'000}, // Glimmer
    {3168101969U, 2817410917U, 10, 200},      // Bright Dust stand-in
}};

constexpr std::array<RankMarker, 2> kRankMarkers{{
    {1808687944U, 3882308435U}, // Valor
    {372496383U, 2772425241U},  // Infamy
}};

/** The Legendary Engram, whose own pool decides the item. */
constexpr std::array<std::uint32_t, 1> kWorldEngram{2223145359U};

/** Black Armory armour the Black Armory gear marker pays a Titan. */
constexpr std::array<std::uint32_t, 5> kBlackArmoryTitanArmour{
    89175933U,
    866063619U,
    4059853946U,
    524862116U,
    2413328031U,
};
/** Black Armory armour the Black Armory gear marker pays a Hunter. */
constexpr std::array<std::uint32_t, 5> kBlackArmoryHunterArmour{
    240988159U,
    2791841721U,
    4106007668U,
    3086191374U,
    3457205569U,
};
/** Black Armory armour the Black Armory gear marker pays a Warlock. */
constexpr std::array<std::uint32_t, 5> kBlackArmoryWarlockArmour{
    3128915572U,
    2153602188U,
    91359169U,
    2389815461U,
    3416654206U,
};

/** Trials weapons the Trials engram pays. */
constexpr std::array<std::uint32_t, 6> kTrialsWeapons{
    1907698332U,
    679281855U,
    958384347U,
    2478792241U,
    1697682876U,
    3164743584U,
};
/** Trials armour the Trials engram pays a Titan. */
constexpr std::array<std::uint32_t, 5> kTrialsTitanArmour{
    435339366U,
    3702689847U,
    1127943297U,
    791799769U,
    2376585692U,
};
/** Trials armour the Trials engram pays a Hunter. */
constexpr std::array<std::uint32_t, 5> kTrialsHunterArmour{
    3275117874U,
    3741471003U,
    938618741U,
    2616071821U,
    18990920U,
};
/** Trials armour the Trials engram pays a Warlock. */
constexpr std::array<std::uint32_t, 5> kTrialsWarlockArmour{
    1637326795U,
    3829990714U,
    2883045518U,
    1215952756U,
    3201140055U,
};

constexpr GearPool kWorldGear{kWorldEngram, {}, {}, {}};
constexpr GearPool kBlackArmoryGear{
    {}, kBlackArmoryTitanArmour, kBlackArmoryHunterArmour, kBlackArmoryWarlockArmour};
constexpr GearPool kTrialsGear{
    kTrialsWeapons, kTrialsTitanArmour, kTrialsHunterArmour, kTrialsWarlockArmour};

constexpr std::array<GearMarker, 8> kGearMarkers{{
    {2127149322U, kWorldGear},       // Legendary Gear
    {3114385605U, kWorldGear},       // Powerful Gear, tier 1
    {3114385606U, kWorldGear},       // Powerful Gear, tier 2
    {3114385607U, kWorldGear},       // Powerful Gear, tier 3
    {73143230U, kWorldGear},         // Pinnacle Gear
    {4039143015U, kWorldGear},       // Powerful Gear without a tier
    {3407672161U, kBlackArmoryGear}, // Black Armory Legendary Gear
    {1526650446U, kTrialsGear},      // Trials engram
}};

/** Each Revelry marker names one armour slot. */
constexpr std::array<ClassArmourMarker, 5> kRevelryArmour{{
    {2501601653U, 1556831535U, 419435523U, 1273510836U},  // Arms
    {514936467U, 1056992393U, 3965417933U, 1376763596U},  // Chest
    {3818379434U, 2328435454U, 2477028154U, 492834021U},  // Head
    {323881355U, 1706874193U, 2764769717U, 1561249470U},  // Legs
    {1508024268U, 2837295684U, 2120905920U, 4075522049U}, // Class item
}};

// --- Reward rows ---------------------------------------------------------------------------

/** What one reward row pays. */
enum class PaymentKind : std::uint8_t {
    nothing,
    experience,
    rankPoints,
    item,
};

/** One reward row's payment, read through the authored tables. */
struct RowPayment {
    PaymentKind kind{PaymentKind::nothing};
    /** The multiple of its cadence's XP an experience row pays. */
    std::int32_t experienceMultiple{};
    /** The progression a rank-point row credits by the row's quantity. */
    std::uint32_t progressionHash{};
    /** The item an item row pays, which may be a wrapper, and its amount. */
    std::uint16_t itemIndex{pursuits::kAbsent};
    std::int32_t quantity{};
};

/** @return The row of an authored table keyed by this item hash, or null. */
template <typename Row, std::size_t Size>
[[nodiscard]] const Row* find_row(const std::array<Row, Size>& rows, std::uint32_t hash) noexcept {
    const auto found =
        std::find_if(rows.begin(), rows.end(), [hash](const Row& row) { return row.hash == hash; });
    return found != rows.end() ? &*found : nullptr;
}

/** @return True when a vendor's endless pool holds the bounty. */
[[nodiscard]] bool in_repeatable_pool(std::uint16_t itemIndex) noexcept {
    std::array<std::uint16_t, build_data::bounties::kPoolCapacity> pool{};
    for (const auto& trigger : build_data::vendors::kRepeatableTriggers) {
        std::size_t count = 0;
        if (!build_data::repeatable_bounty_pool(trigger.itemType, pool, count)) {
            continue;
        }
        const auto members = std::span(pool).first(count);
        if (std::find(members.begin(), members.end(), itemIndex) != members.end()) {
            return true;
        }
    }
    return false;
}

/** @return The cadence a vendor's endless pool gives the pursuit, or else its lifetime. */
[[nodiscard]] Cadence pursuit_cadence(std::uint16_t itemIndex,
                                      std::int32_t lifetimeSeconds) noexcept {
    if (in_repeatable_pool(itemIndex)) {
        return Cadence::repeatable;
    }
    if (lifetimeSeconds >= kWeekSeconds) {
        return Cadence::weekly;
    }
    return lifetimeSeconds > 0 ? Cadence::daily : Cadence::none;
}

/** @return Seasonal XP an experience marker pays at this cadence. */
[[nodiscard]] constexpr std::int32_t cadence_experience(Cadence cadence) noexcept {
    switch (cadence) {
    case Cadence::repeatable:
        return kRepeatableBountyExperience;
    case Cadence::daily:
        return kDailyBountyExperience;
    case Cadence::weekly:
        return kWeeklyBountyExperience;
    case Cadence::none:
        break;
    }
    return 0;
}

/** @return The armour one class draws from a pool. */
[[nodiscard]] std::span<const std::uint32_t> class_armour(const GearPool& pool,
                                                          CharacterClass characterClass) noexcept {
    switch (characterClass) {
    case CharacterClass::titan:
        return pool.titanArmour;
    case CharacterClass::hunter:
        return pool.hunterArmour;
    case CharacterClass::warlock:
        return pool.warlockArmour;
    }
    return {};
}

/**
 * Draws one installed item from a pool, starting at the seeded member.
 * @return False when no member is installed.
 */
[[nodiscard]] bool draw_gear(const GearPool& pool,
                             CharacterClass characterClass,
                             std::uint64_t seed,
                             std::uint16_t& itemIndex) noexcept {
    const std::span<const std::uint32_t> armour = class_armour(pool, characterClass);
    const std::size_t size = pool.weapons.size() + armour.size();
    for (std::size_t offset = 0; offset < size; ++offset) {
        const std::size_t member = static_cast<std::size_t>((seed + offset) % size);
        const std::uint32_t hash = member < pool.weapons.size()
                                       ? pool.weapons[member]
                                       : armour[member - pool.weapons.size()];
        build_data::items::Definition item{};
        if (build_data::find_item_definition_hash(hash, item)) {
            itemIndex = item.definitionIndex;
            return true;
        }
    }
    return false;
}

/**
 * Resolves the one item a gear marker pays.
 * @return False when nothing the marker names is installed.
 */
[[nodiscard]] bool pay_gear(const GearMarker* marker,
                            const ClassArmourMarker* armourMarker,
                            CharacterClass characterClass,
                            std::uint64_t seed,
                            std::uint16_t& itemIndex) noexcept {
    if (marker != nullptr) {
        return draw_gear(marker->pool, characterClass, seed, itemIndex);
    }
    const std::array<std::uint32_t, 1> titan{armourMarker->titanArmour};
    const std::array<std::uint32_t, 1> hunter{armourMarker->hunterArmour};
    const std::array<std::uint32_t, 1> warlock{armourMarker->warlockArmour};
    return draw_gear({{}, titan, hunter, warlock}, characterClass, seed, itemIndex);
}

/**
 * Reads what one reward row pays. The authored tables decide a marker or currency they name; any
 * other row pays its own item, unless it has no amount or names a stand-in.
 * @param seed Draws a gear marker's item.
 * @return False when a gear marker or currency names nothing installed.
 */
[[nodiscard]] bool row_payment(const pursuits::RewardPreview& reward,
                               Cadence cadence,
                               CharacterClass characterClass,
                               std::uint64_t seed,
                               RowPayment& payment) noexcept {
    payment = {};
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(reward.itemIndex, item)) {
        // The draw refuses the unknown item.
        if (reward.quantity != 0) {
            payment = {PaymentKind::item, 0, 0, reward.itemIndex, reward.quantity};
        }
        return true;
    }
    const std::uint32_t hash = item.definitionHash;
    if (hash == kExperienceMarkerHash || hash == kDoubleExperienceMarkerHash) {
        payment = {PaymentKind::experience, hash == kDoubleExperienceMarkerHash ? 2 : 1};
        return true;
    }
    if (const RankMarker* marker = find_row(kRankMarkers, hash)) {
        if (reward.quantity > 0) {
            payment = {PaymentKind::rankPoints, 0, marker->progressionHash};
        }
        return true;
    }
    if (const CurrencyPayout* payout = find_row(kCurrencyPayouts, hash)) {
        std::int32_t quantity = reward.quantity;
        if (quantity == 0 && cadence != Cadence::none) {
            quantity = cadence == Cadence::weekly ? payout->weeklyQuantity : payout->dailyQuantity;
        }
        if (quantity == 0) {
            return true;
        }
        if (!build_data::find_item_definition_hash(payout->currencyHash, item)) {
            return false;
        }
        payment = {PaymentKind::item, 0, 0, item.definitionIndex, quantity};
        return true;
    }
    const GearMarker* gear = find_row(kGearMarkers, hash);
    const ClassArmourMarker* armour = find_row(kRevelryArmour, hash);
    if (gear != nullptr || armour != nullptr) {
        payment = {PaymentKind::item, 0, 0, pursuits::kAbsent, 1};
        return pay_gear(gear, armour, characterClass, seed, payment.itemIndex);
    }
    if (reward.quantity != 0 && !names_stand_in(reward.itemIndex)) {
        payment = {PaymentKind::item, 0, 0, reward.itemIndex, reward.quantity};
    }
    return true;
}

/**
 * Credits one rank-point row: the progression's progress moves by the row's quantity and stops at
 * the total its ranks cost.
 * @return False when the progression is not installed or replicated, or a second one is credited.
 */
[[nodiscard]] bool plan_rank_points(std::uint32_t progressionHash,
                                    std::int32_t quantity,
                                    unlocks::Table& banks,
                                    CompletionPlan& plan) noexcept {
    progressions::Definition definition{};
    std::array<progressions::Step, progressions::kStepPerDefinitionCapacity> steps{};
    std::size_t stepCount = 0;
    if (!build_data::find_progression_definition_hash(progressionHash, definition)
        || definition.scope == progressions::Scope::unreplicated
        || !build_data::find_progression_steps(definition.definitionIndex, steps, stepCount)) {
        return false;
    }
    PreparedProgressionCredit& credit = plan.progression;
    if (credit.scope == progressions::Scope::unreplicated) {
        credit = {definition.scope, definition.definitionIndex};
        credit.previousValue = progression_value(banks, credit);
        credit.value = credit.previousValue;
    } else if (credit.scope != definition.scope
               || credit.definitionIndex != definition.definitionIndex) {
        return false;
    }
    std::int64_t total = 0;
    for (const progressions::Step& step : std::span(steps).first(stepCount)) {
        total += step.cost;
    }
    if (credit.value < total) {
        const std::int64_t credited = std::int64_t{credit.value} + quantity;
        credit.value = static_cast<std::int32_t>((std::min)(credited, total));
    }
    return true;
}

/**
 * Resolves the reward rows a pursuit shows into items, seasonal XP and a progression credit. A
 * wrapper draws from its own pool.
 * @param pursuitIndex The pursuit's item index, which with its lifetime gives its cadence.
 * @param context Saved banks and class of the redeeming character; its seed starts the draws.
 * @param banks The same saved banks, which a progression credit reads its progress from.
 * @return False when a row is conditional, draws nothing, or outgrows the grant.
 */
[[nodiscard]] bool plan_rewards(std::uint16_t pursuitIndex,
                                const pursuits::Definition& contract,
                                const reward_resolution::Context& context,
                                unlocks::Table& banks,
                                CompletionPlan& plan,
                                const char*& reason) noexcept {
    const Cadence cadence = pursuit_cadence(pursuitIndex, contract.lifetimeSeconds);
    std::int32_t experienceMultiple = 0;
    const auto rewards = std::span(contract.rewards).first(contract.rewardCount);
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        const pursuits::RewardPreview& reward = rewards[index];
        reward_resolution::Context rowContext = context;
        rowContext.seed = context.seed + index;
        RowPayment payment{};
        reason = "reward_marker";
        if (!row_payment(reward, cadence, context.characterClass, rowContext.seed, payment)) {
            return false;
        }
        if (payment.kind == PaymentKind::nothing) {
            continue;
        }
        reason = "reward_condition";
        if (reward.conditionIndex != pursuits::kAbsent) {
            return false;
        }
        if (payment.kind == PaymentKind::experience) {
            // Several experience rows describe one payout.
            experienceMultiple = (std::max)(experienceMultiple, payment.experienceMultiple);
            continue;
        }
        reason = "reward_rank_points";
        if (payment.kind == PaymentKind::rankPoints) {
            if (!plan_rank_points(payment.progressionHash, reward.quantity, banks, plan)) {
                return false;
            }
            continue;
        }
        reason = nullptr;
        reward_resolution::Result resolved{};
        if (reward_resolution::resolve(rowContext,
                                       payment.itemIndex,
                                       static_cast<std::uint32_t>(payment.quantity),
                                       resolved)
            != reward_resolution::Resolution::resolved) {
            if (reason == nullptr) {
                reason = "reward_draw";
            }
            return false;
        }
        reason = "reward_capacity";
        if (resolved.count > plan.grants.size() - plan.rowCount) {
            return false;
        }
        plan.wrapperFlags |= resolved.wrapperFlags;
        const auto grants = std::span(resolved.grants).first(resolved.count);
        for (const reward_resolution::Grant& grant : grants) {
            plan.grants[plan.rowCount] = grant;
            const reward_resolution::Grant& kept = plan.grants[plan.rowCount];
            plan.rows[plan.rowCount++] = {kept.itemIndex,
                                          kept.quantity,
                                          std::span(kept.sockets).first(kept.socketCount),
                                          true};
        }
    }
    plan.seasonalExperience = cadence_experience(cadence) * experienceMultiple;
    return true;
}

} // namespace

/** Judges completion and plans under one store lock, then prepares the grant from that plan. */
PursuitRedemption prepare_pursuit_redemption(std::uint64_t instanceSoid,
                                             std::uint32_t definitionHash,
                                             std::int64_t nowSeconds,
                                             std::uint64_t seed,
                                             PendingRecordRewardGrant& mutation,
                                             const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "not_ready";
    if (!build_data::pursuit_definitions_ready() || !build_data::reward_definitions_ready()
        || !build_data::progression_definitions_ready()
        || !build_data::repeatable_bounties_ready()) {
        return PursuitRedemption::notReady;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    const std::unique_ptr<AccountState> account(new (std::nothrow) AccountState{});
    const std::unique_ptr<unlocks::Table> banks(new (std::nothrow) unlocks::Table{});
    reason = "saved_state";
    if (!account || !banks || !investment::store::read_account(*account)) {
        return PursuitRedemption::notReady;
    }
    const std::size_t characterIndex = selected_character_index(*account);
    if (characterIndex >= account->characterCount
        || !investment::store::read_unlocks(*banks, static_cast<int>(characterIndex))) {
        return PursuitRedemption::notReady;
    }
    const CharacterState& character = account->characters[characterIndex];
    const auto held = std::span(character.inventory.values).first(character.inventory.count);
    const auto source = std::find_if(held.begin(), held.end(), [instanceSoid](const auto& item) {
        return item.instanceSoid == instanceSoid;
    });
    build_data::items::Definition item{};
    pursuits::Definition contract{};
    reason = "pursuit";
    if (instanceSoid == 0 || source == held.end() || source->definitionHash != definitionHash
        || source->quantity != 1
        || !build_data::find_item_definition_hash(source->definitionHash, item)
        || !build_data::find_pursuit_definition(item.definitionIndex, contract)
        || !contract.readable) {
        return PursuitRedemption::refused;
    }
    reason = "locked";
    if ((source->flags & authored_inventory::kLockedItemFlag) != 0) {
        return PursuitRedemption::refused;
    }
    static constexpr authored_inventory::ItemObjectives kUnsetItemValues{};
    const authored_inventory::ItemObjectives& itemValues =
        source->objectives.has_value() ? *source->objectives : kUnsetItemValues;
    reason = "expired";
    if (contract.lifetimeSeconds > 0) {
        const std::int32_t deadline = itemValues[authored_inventory::kItemExpiryLane];
        if (deadline <= 0 || nowSeconds >= deadline) {
            return PursuitRedemption::notRedeemable;
        }
    }

    const reward_resolution::Context context{*banks, character.characterClass, seed, &reason};
    const PursuitCompletionStatus status =
        inspect_pursuit_completion(contract, context, itemValues);
    const pursuits::QuestStep& step = contract.questStep;
    reason = "quest_progress";
    if (step.membership != pursuits::QuestMembership::none && !progress_item_held(status)) {
        return PursuitRedemption::notRedeemable;
    }
    reason = "objective_source";
    if (!status.objectivesKnown) {
        return PursuitRedemption::refused;
    }
    reason = "objective_reads_deadline";
    if (reads_deadline(contract)) {
        return PursuitRedemption::refused;
    }
    reason = "incomplete";
    if (!status.objectivesComplete) {
        return PursuitRedemption::notRedeemable;
    }

    CompletionPlan plan{};
    reason = "quest_step";
    if (step.membership == pursuits::QuestMembership::unsupported
        || (step.membership == pursuits::QuestMembership::step
            && !plan_quest_step(step, *banks, plan))) {
        return PursuitRedemption::refused;
    }
    reason = "reward_rows";
    const bool paysRewards = step.membership == pursuits::QuestMembership::none
                             || step.successorIndex == pursuits::kAbsent;
    if (paysRewards
        && (!contract.rewardsReadable
            || !plan_rewards(item.definitionIndex, contract, context, *banks, plan, reason))) {
        return PursuitRedemption::refused;
    }

    const RewardPreparation grant =
        prepare_reward_grant(std::span(plan.rows).first(plan.rowCount),
                             kUnclaimedRecordIndex,
                             &plan.wrapperFlags,
                             instanceSoid,
                             mutation,
                             &reason);
    if (grant != RewardPreparation::prepared) {
        return grant == RewardPreparation::notReady ? PursuitRedemption::notReady
                                                    : PursuitRedemption::refused;
    }
    // A step moved on without its item would leave the quest with no step to hold.
    reason = "quest_successor_placement";
    if (plan.questStep.next.scope != Quest::Scope::none
        && mutation.rewards[0].placedQuantity == 0) {
        return PursuitRedemption::refused;
    }
    mutation.questStep = plan.questStep;
    mutation.progression = plan.progression;
    mutation.seasonalExperience = plan.seasonalExperience;
    reason = nullptr;
    return PursuitRedemption::prepared;
}

} // namespace sunrise::state
