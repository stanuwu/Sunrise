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
#include <optional>
#include <span>

#include "../../core/logging/log.h"
#include "../account/inventory/synthesizer_tier.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
#include "pursuit_completion_runtime.h"
#include "runtime.h"
#include "saved_selection_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace inventory_buckets = build_data::inventory::buckets;
namespace progression_data = build_data::progressions;
namespace pursuit_data = build_data::pursuits;
using Quest = build_data::items::QuestInitialization;

static_assert(pursuit_data::kItemValueCapacity == authored_inventory::kItemObjectiveLaneCount);

namespace {

/**
 * What redeeming one pursuit grants: the pursuit leaves, its quest set's saved step moves to the
 * next member, and that member is granted. Only the last member, or a pursuit outside any set,
 * grants the reward rows it shows, so a set pays them once.
 */
struct CompletionPlan {
    std::array<DirectRecordReward, kRecordRewardGrantCapacity> rows{};
    std::size_t rowCount{};
    std::bitset<unlocks::kAccountFlagCapacity> wrapperFlags{};
    std::array<reward_resolution::Grant, kRecordRewardGrantCapacity> grants{};
    /** The completion values the contract names, each set by the redemption. */
    PreparedQuestStep completionValue{};
    PreparedQuestStep completionContextValue{};
    PreparedQuestStep questStep{};
    PreparedProgressionCredit progression{};
    std::int32_t seasonalExperience{};
    /** The character's Gambit Prime helmet tiers after the grant. */
    std::array<GambitPrimeHelmetTier, kGambitPrimeRoleCount> helmetTiers{};
    /** The held Synthesizer the next tier replaces, or zero. */
    std::uint64_t replacedSynthesizerSoid{};
    /** The treasure map a Werner bounty pays in place of the rows it shows, if any. */
    std::uint32_t treasureMapHash{authored_inventory::kNoDefinitionHash};
};

/**
 * A reward row names a stand-in when its item sits in a one-row FIFO bucket, or in the perk bucket
 * with no acquisition flag. The authored tables pay the stand-ins they name; any other is logged
 * and grants nothing. A Dawning ingredient pickup is placed by the grant, so it is no stand-in.
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

/** Completing a pursuit sets each completion value it names to this. */
constexpr std::int32_t kCompletionValueSet = 1;

/**
 * Plans setting one completion value, observing what the saved banks hold.
 * @return False when the contract names a value that maps to no saved row.
 */
[[nodiscard]] bool plan_completion_value(const pursuit_data::CompletionValue& value,
                                         unlocks::Table& banks,
                                         PreparedQuestStep& write) noexcept {
    if (!value.declared) {
        return true;
    }
    if (value.scope == Quest::Scope::none) {
        return false;
    }
    write.next = {kCompletionValueSet, value.row, value.scope};
    write.previousValue = quest_value(banks, write.next);
    return true;
}

/**
 * A bounty's deadline is its first per-item value, so an objective read from that value would
 * count the deadline as progress.
 */
[[nodiscard]] bool reads_deadline(const pursuit_data::Definition& contract) noexcept {
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
[[nodiscard]] bool plan_quest_step(const pursuit_data::QuestStep& step,
                                   unlocks::Table& banks,
                                   CompletionPlan& plan) noexcept {
    const std::int32_t saved = quest_value(banks, {step.value, step.row, step.scope});
    if (saved != step.value) {
        return false;
    }
    if (step.successorIndex != pursuit_data::kAbsent) {
        plan.questStep = {{step.successorValue, step.row, step.scope}, saved};
        plan.rows[plan.rowCount++] = {step.successorIndex, 1};
    }
    return true;
}

/** How often a bounty can be taken, which scales what its markers and currencies pay. */
enum class Cadence : std::uint8_t {
    /** Nothing names the bounty's cadence and it never expires. */
    none,
    repeatable,
    daily,
    weekly,
};

/** The source a bounty's gear markers draw from. A source with no gear pool draws world gear. */
enum class GearSource : std::uint8_t {
    world,
    blackArmoryArmour,
    blackArmoryForge,
    revelry,
    dreamingCity,
    gambit,
    gambitPrimeWeapons,
    ironBanner,
    lastWish,
    scourge,
    crown,
    trials,
    /**
     * Gambit Prime role weeklies: their role marker pays the next helmet tier, and their
     * Synthesizer Upgrade marker the next Synthesizer.
     */
    gambitPrimeRole,
};

/** Bounties that share a cadence and a gear source. */
struct BountyFamily {
    std::span<const std::uint32_t> bounties;
    /** None when the bounty's lifetime decides. */
    Cadence cadence{};
    GearSource gear{};
};

/** What one bounty's markers read: its cadence and the source its gear draws from. */
struct BountyRules {
    Cadence cadence{};
    GearSource gear{};
};

/** A Werner bounty pays its treasure map in place of the rows it shows. */
struct TreasureMapTransition {
    /** The bounty. */
    std::uint32_t hash{};
    std::uint32_t mapHash{};
};

/** Items a gear marker draws from: weapons any class draws and each class's armour. */
struct GearPool {
    std::span<const std::uint32_t> weapons;
    std::span<const std::uint32_t> titanArmour;
    std::span<const std::uint32_t> hunterArmour;
    std::span<const std::uint32_t> warlockArmour;
};

/** A gear source and the pool it draws from. */
struct SourcePool {
    GearSource source{};
    GearPool pool{};
};

/** A Revelry gear marker, which pays one fixed armour piece per class. */
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

/** Helmet tiers a Gambit Prime role pays, Illicit to Notorious. */
constexpr std::size_t kHelmetTierCount =
    static_cast<std::size_t>(GambitPrimeHelmetTier::notorious);

/** A Gambit Prime role marker and its helmets, by class in CharacterClass order, then by tier. */
struct RoleHelmetMarker {
    std::uint32_t hash{};
    std::array<std::array<std::uint32_t, kHelmetTierCount>, kCharacterClassCount> classTiers{};
};

/** @return The row of an authored table keyed by this item hash, or null. */
template <typename Row, std::size_t Size>
[[nodiscard]] const Row* find_row(const std::array<Row, Size>& rows, std::uint32_t hash) noexcept {
    const auto found =
        std::find_if(rows.begin(), rows.end(), [hash](const Row& row) { return row.hash == hash; });
    return found != rows.end() ? &*found : nullptr;
}

/** @return True when the authored list holds this hash. */
[[nodiscard]] bool lists(std::span<const std::uint32_t> hashes, std::uint32_t hash) noexcept {
    return std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
}

// --- Authored game data --------------------------------------------------------------------

// TODO: extract what these markers and bounties pay; no extractor relates a reward marker or a
// bounty to the experience, currency, progression or loot the retail server paid for it.

/** Seasonal XP an experience marker pays, by cadence. */
constexpr std::int32_t kRepeatableBountyExperience = 4'000;
constexpr std::int32_t kDailyBountyExperience = 6'000;
constexpr std::int32_t kWeeklyBountyExperience = 12'000;

/** A weekly bounty lasts seven days and a daily one 27 hours; anything past 36 hours is weekly. */
constexpr std::int32_t kWeeklyLifetimeFloorSeconds = 36 * 60 * 60;

/** The double experience marker pays twice its cadence's XP. */
constexpr std::uint32_t kExperienceMarkerHash = 2211488305U;
constexpr std::uint32_t kDoubleExperienceMarkerHash = 583797518U;

/**
 * Glimmer and Bright Dust are named twice: a reward row names the one-unit display item, and the
 * wallet holds the other. A zero-amount row pays the amount the retail bounties paid.
 */
constexpr std::array<CurrencyPayout, 10> kCurrencyPayouts{{
    {3085039018U, 3159615086U, 1'000, 3'000}, // Glimmer stand-in
    {3159615086U, 3159615086U, 1'000, 3'000}, // Glimmer
    {3168101969U, 2817410917U, 10, 200},      // Bright Dust stand-in
    {3853748946U, 3853748946U, 1, 1},         // Enhancement Core, Banshee's dailies
    {592227263U, 592227263U, 10, 25},         // Baryon Bough
    {2979281381U, 2979281381U, 1, 1},         // Upgrade Module
    {1633854071U, 1633854071U, 1, 1},         // Dark Fragment
    {1531478589U, 1531478589U, 1, 1},         // Offering to the Oracle
    {1165306707U, 1165306707U, 1, 1},         // Hymn of Desecration
    {3255036626U, 3255036626U, 1, 1},         // Transcendent Blessing
}};

constexpr std::array<RankMarker, 2> kRankMarkers{{
    {1808687944U, 3882308435U}, // Valor
    {372496383U, 2772425241U},  // Infamy
}};

/** Gear markers, which draw from their bounty's source. */
constexpr std::array<std::uint32_t, 8> kGearMarkers{
    2127149322U, // Legendary Gear
    3407672161U, // Black Armory Legendary Gear
    3114385605U, // Powerful Gear, tier 1
    3114385606U, // Powerful Gear, tier 2
    3114385607U, // Powerful Gear, tier 3
    73143230U,   // Pinnacle Gear
    4039143015U, // Powerful Gear without a tier
    1526650446U, // Trials Engram
};

/** Each Revelry marker names one armour slot. */
constexpr std::array<ClassArmourMarker, 5> kRevelryArmour{{
    {2501601653U, 1556831535U, 419435523U, 1273510836U},  // Arms
    {514936467U, 1056992393U, 3965417933U, 1376763596U},  // Chest
    {3818379434U, 2328435454U, 2477028154U, 492834021U},  // Head
    {323881355U, 1706874193U, 2764769717U, 1561249470U},  // Legs
    {1508024268U, 2837295684U, 2120905920U, 4075522049U}, // Class item
}};

/** Role markers in GambitPrimeRole order. */
constexpr std::array<RoleHelmetMarker, kGambitPrimeRoleCount> kRoleHelmets{{
    {3007303932U, // Reaper
     {{{223681334U, 223681335U, 223681332U},
       {1295793306U, 1295793307U, 1295793304U},
       {1208982393U, 1208982392U, 1208982395U}}}},
    {1355700046U, // Invader
     {{{3636943394U, 3636943395U, 3636943392U},
       {2593076934U, 2593076935U, 2593076932U},
       {1951201411U, 1951201410U, 1951201409U}}}},
    {4041437604U, // Collector
     {{{975478396U, 975478397U, 975478398U},
       {2698109344U, 2698109345U, 2698109346U},
       {2565812705U, 2565812704U, 2565812707U}}}},
    {1045201464U, // Sentry
     {{{2187982746U, 2187982747U, 2187982744U},
       {3220030414U, 3220030415U, 3220030412U},
       {3660501109U, 3660501108U, 3660501111U}}}},
}};

/** The Synthesizer Upgrade marker, which replaces the held Synthesizer with the next tier. */
constexpr std::uint32_t kSynthesizerUpgradeMarkerHash = 596773932U;

/** The Legendary Rune marker, which pays nothing. */
constexpr std::uint32_t kLegendaryRuneMarkerHash = 1772646107U;

/** Repeatable bounties. */
constexpr std::array<std::uint32_t, 159> kRepeatableBounties{
    3801019451U, 3801019450U, 3801019449U, 3801019448U, 3801019455U, 3801019454U, 3801019453U,
    3801019452U, 3801019443U, 3801019442U, 4228939450U, 4228939451U, 4228939448U, 4228939449U,
    4228939454U, 4228939455U, 4228939452U, 4228939453U, 4228939442U, 1932203931U, 1486111804U,
    782388144U, 3609320918U, 3089242929U, 1494697569U, 789196172U, 3512551717U, 2121605528U,
    711780003U, 3529619172U, 1387738947U, 3292943447U, 1491123344U, 3503381224U, 2212074115U,
    833201720U, 2514375981U, 2420899653U, 729780848U, 3112855567U, 90105468U, 429152515U,
    4104555523U, 1115261524U, 1723091344U, 1400452240U, 3658765719U, 3351200278U, 2731635990U,
    3675498641U, 3636655077U, 1777618773U, 484780086U, 1774447862U, 4059065847U, 903424492U,
    2940269474U, 4235693605U, 211068288U, 2462005227U, 3109359405U, 3109359404U, 3109359407U,
    3109359406U, 3109359401U, 3109359400U, 3109359403U, 3109359402U, 3109359397U, 3109359396U,
    204755896U, 204755897U, 204755898U, 204755899U, 204755900U, 204755901U, 204755902U,
    2592950299U, 2592950298U, 2592950297U, 2592950296U, 2592950303U, 2592950302U, 2592950301U,
    2592950300U, 2592950291U, 2592950290U, 1718124766U, 1718124767U, 1718124764U, 1718124765U,
    1718124762U, 1718124763U, 2805818748U, 3990791875U, 3683313874U, 3514703992U, 3512622966U,
    692493341U, 13409814U, 3312152065U, 1913307766U, 3357287242U, 391818767U, 3134246761U,
    2703627837U, 2816193016U, 874160405U, 146278979U, 4114666828U, 1315013095U, 3155878395U,
    2653579669U, 2428565738U, 3141795163U, 3696295486U, 1453527970U, 2133176491U, 3697234118U,
    3178157690U, 2068952036U, 3191735197U, 2675137426U, 1244249306U, 3038846813U, 2695042788U,
    857633000U, 3971836963U, 3731062163U, 1812672687U, 501758776U, 2903204355U, 3560267478U,
    3209637560U, 1428740103U, 1258056755U, 2497038231U, 1113031720U, 2003717903U, 1983539982U,
    1058022683U, 1899370350U, 1561597496U, 2285406418U, 31218063U, 310094086U, 3219965079U,
    2213620835U, 711490543U, 2267981985U, 130487539U, 2328867053U, 4084493733U, 2858316395U,
    3438972653U, 847805104U, 3721656876U, 1764928973U, 341098154U,
};
/** Petra's daily bounties. */
constexpr std::array<std::uint32_t, 6> kDreamingCityDailyBounties{
    1391363409U,
    2662637005U,
    4095766153U,
    2271002093U,
    2494871935U,
    1912981534U,
};
/** Petra's weekly bounties, Audience with the Queen included. */
constexpr std::array<std::uint32_t, 7> kDreamingCityWeeklyBounties{
    3337739523U,
    128980839U,
    1147672297U,
    2338128705U,
    3207732940U,
    542328999U,
    871673916U,
};
/** Trials daily bounties. */
constexpr std::array<std::uint32_t, 18> kTrialsDailyBounties{
    1807819922U, 2719669554U, 2469435739U, 1649087790U, 3647168216U, 3636568846U, 2849007504U,
    3238546247U, 836380479U, 712137159U, 303384168U, 2398816281U, 4126111451U, 550008052U,
    1761112788U, 2943701307U, 1244906310U, 1083494845U,
};
/** Trials weekly bounties other than End Game. */
constexpr std::array<std::uint32_t, 8> kTrialsWeeklyBounties{
    3051422226U,
    3497386880U,
    42252934U,
    58287472U,
    225464052U,
    1148638263U,
    2780182450U,
    3099386391U,
};
/** End Game, whose engram draws from the whole Trials pool. */
constexpr std::array<std::uint32_t, 1> kTrialsEndGameBounties{
    45262603U,
};
/** Hawthorne's clan weeklies; A Calus Affair names the un-tiered Powerful Gear marker. */
constexpr std::array<std::uint32_t, 9> kHawthorneBounties{
    2655712924U,
    459676116U,
    852248609U,
    1794825180U,
    2850564884U,
    2729271716U,
    1441461394U,
    1381806136U,
    504379403U,
};
/** Ada-1's weeklies that pay Black Armory armour. */
constexpr std::array<std::uint32_t, 2> kBlackArmoryArmourBounties{
    1128713411U,
    3179992453U,
};
/** Ada-1's rare bounties, which pay forge weapons or Black Armory armour. */
constexpr std::array<std::uint32_t, 8> kBlackArmoryForgeBounties{
    89995072U,
    346974120U,
    774885924U,
    1085775425U,
    1126196126U,
    1727646073U,
    3357615447U,
    4237578370U,
};
/** Revelry weeklies; each one's marker names the armour slot. */
constexpr std::array<std::uint32_t, 6> kRevelryBounties{
    78926733U,
    3180273074U,
    3180273075U,
    3180273072U,
    3180273073U,
    3180273078U,
};
/** Iron Banner weeklies. */
constexpr std::array<std::uint32_t, 7> kIronBannerBounties{
    3063752398U,
    2099765145U,
    1960358731U,
    4068460499U,
    1284302942U,
    3780376499U,
    2352873759U,
};
/** Luna's Recall. */
constexpr std::array<std::uint32_t, 1> kLunasRecallBounties{
    1264945960U,
};
/** Purification Ritual bounties. */
constexpr std::array<std::uint32_t, 5> kPurificationBounties{
    346263927U,
    1123124244U,
    2256480378U,
    455233900U,
    2663541547U,
};
/** The Drifter's ordinary weeklies. */
constexpr std::array<std::uint32_t, 3> kGambitBounties{
    2839632402U,
    2839632400U,
    2839632405U,
};
/** Yes Sir, I'm a Closer, which pays a Gambit Prime weapon. */
constexpr std::array<std::uint32_t, 1> kGambitPrimeWeaponBounties{
    4132751916U,
};
/** Gambit Prime role weeklies. */
constexpr std::array<std::uint32_t, 8> kGambitPrimeRoleBounties{
    53973468U,
    2724713067U,
    53973469U,
    2496824396U,
    53973470U,
    1417485797U,
    53973471U,
    535212982U,
};
/** Hawthorne's Last Wish challenges. */
constexpr std::array<std::uint32_t, 5> kLastWishBounties{
    2836954349U,
    1250327262U,
    3871581136U,
    1568895666U,
    4007940282U,
};
/** Hawthorne's Scourge of the Past challenges. */
constexpr std::array<std::uint32_t, 3> kScourgeBounties{
    1348944144U,
    3415614992U,
    1381881897U,
};
/** Hawthorne's Crown of Sorrow challenges. */
constexpr std::array<std::uint32_t, 3> kCrownBounties{
    2459033425U,
    2459033426U,
    2459033427U,
};
/** Forge weapons. */
constexpr std::array<std::uint32_t, 7> kBlackArmoryForgeWeapons{
    603242241U,
    93253474U,
    3843477312U,
    2575506895U,
    421573768U,
    1449922174U,
    3704653637U,
};
/** Dreaming City weapons. */
constexpr std::array<std::uint32_t, 7> kDreamingCityWeapons{
    640114618U,
    334171687U,
    3242168339U,
    3297863558U,
    346136302U,
    3740842661U,
    1644160541U,
};
/** Dreaming City armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kDreamingCityTitanArmour{
    2503434573U,
    4070309619U,
    1980768298U,
    4097166900U,
    3174233615U,
};
/** Dreaming City armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kDreamingCityHunterArmour{
    1705856569U,
    1593474975U,
    3306564654U,
    2824453288U,
    344548395U,
};
/** Dreaming City armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kDreamingCityWarlockArmour{
    2761343386U,
    2859583726U,
    3602032567U,
    185695659U,
    188778964U,
};
/** Gambit weapons. */
constexpr std::array<std::uint32_t, 8> kGambitWeapons{
    1789347249U,
    4077196130U,
    2712244741U,
    2034817450U,
    2653316158U,
    2217366863U,
    3100452337U,
    991314988U,
};
/** Gambit armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kGambitTitanArmour{
    3863492689U,
    1811579911U,
    2993008662U,
    2002452096U,
    3694642467U,
};
/** Gambit armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kGambitHunterArmour{
    606902507U,
    1314666277U,
    436615288U,
    1267361154U,
    1698660093U,
};
/** Gambit armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kGambitWarlockArmour{
    4122447870U,
    725297842U,
    896081219U,
    754069623U,
    671423576U,
};
/** Iron Banner weapons. */
constexpr std::array<std::uint32_t, 8> kIronBannerWeapons{
    1690783811U,
    65611680U,
    136525518U,
    1972985595U,
    1982711279U,
    3169616514U,
    2108920981U,
    4292849692U,
};
/** Iron Banner armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kIronBannerTitanArmour{
    391384020U,
    3517179757U,
    1571959827U,
    3763521327U,
    4201843274U,
};
/** Iron Banner armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kIronBannerHunterArmour{
    3624199242U,
    3671337107U,
    4039932861U,
    3406291173U,
    1248530160U,
};
/** Iron Banner armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kIronBannerWarlockArmour{
    2717305289U,
    125833536U,
    2522706952U,
    3115740538U,
    4224804453U,
};
/** Last Wish weapons. */
constexpr std::array<std::uint32_t, 8> kLastWishWeapons{
    2721249463U,
    601592879U,
    4094657108U,
    654370424U,
    568515759U,
    3799980700U,
    686951703U,
    2545083870U,
};
/** Last Wish armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kLastWishTitanArmour{
    576683388U,
    3143067364U,
    16387641U,
    4219088013U,
    3874578566U,
};
/** Last Wish armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kLastWishHunterArmour{
    1127835600U,
    2868042232U,
    1646520469U,
    1190016345U,
    1444894250U,
};
/** Last Wish armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kLastWishWarlockArmour{
    3492720019U,
    776723133U,
    2280287728U,
    3445582154U,
    3227674085U,
};
/** Scourge of the Past weapons. */
constexpr std::array<std::uint32_t, 4> kScourgeWeapons{
    2753269585U,
    2186258845U,
    1931556011U,
    1664372054U,
};
/** Scourge of the Past armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kScourgeTitanArmour{
    1989682895U,
    3491990569U,
    977326564U,
    2719710110U,
    2564183153U,
};
/** Scourge of the Past armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kScourgeHunterArmour{
    2334017923U,
    300528205U,
    2750983488U,
    96643258U,
    384384821U,
};
/** Scourge of the Past armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kScourgeWarlockArmour{
    2286640864U,
    4092373800U,
    1499503877U,
    583145321U,
    940003738U,
};
/** Crown of Sorrow weapons. */
constexpr std::array<std::uint32_t, 4> kCrownWeapons{
    1286686760U,
    1496419775U,
    2338088853U,
    3861448240U,
};
/** Crown of Sorrow armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kCrownTitanArmour{
    1595987387U,
    3406713877U,
    2104205416U,
    1129634130U,
    4450861U,
};
/** Crown of Sorrow armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kCrownHunterArmour{
    4017853847U,
    942205921U,
    2149271612U,
    326149062U,
    1107067065U,
};
/** Crown of Sorrow armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kCrownWarlockArmour{
    3211894260U,
    3381758732U,
    1319515713U,
    2472794149U,
    3499632894U,
};
/** Gambit Prime and Reckoning weapons. */
constexpr std::array<std::uint32_t, 10> kGambitPrimeWeapons{
    2744715540U,
    736901634U,
    821154603U,
    715338174U,
    755130877U,
    2199171672U,
    3504336176U,
    299665907U,
    1115104187U,
    3116356268U,
};

/** World gear is the Legendary Engram, whose own pool decides the item. */
constexpr std::array<std::uint32_t, 1> kWorldEngram{2223145359U};

/** Black Armory armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kBlackArmoryTitanArmour{
    89175933U,
    866063619U,
    4059853946U,
    524862116U,
    2413328031U,
};
/** Black Armory armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kBlackArmoryHunterArmour{
    240988159U,
    2791841721U,
    4106007668U,
    3086191374U,
    3457205569U,
};
/** Black Armory armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kBlackArmoryWarlockArmour{
    3128915572U,
    2153602188U,
    91359169U,
    2389815461U,
    3416654206U,
};

/** Trials weapons. */
constexpr std::array<std::uint32_t, 6> kTrialsWeapons{
    1907698332U,
    679281855U,
    958384347U,
    2478792241U,
    1697682876U,
    3164743584U,
};
/** Trials armour for a Titan. */
constexpr std::array<std::uint32_t, 5> kTrialsTitanArmour{
    435339366U,
    3702689847U,
    1127943297U,
    791799769U,
    2376585692U,
};
/** Trials armour for a Hunter. */
constexpr std::array<std::uint32_t, 5> kTrialsHunterArmour{
    3275117874U,
    3741471003U,
    938618741U,
    2616071821U,
    18990920U,
};
/** Trials armour for a Warlock. */
constexpr std::array<std::uint32_t, 5> kTrialsWarlockArmour{
    1637326795U,
    3829990714U,
    2883045518U,
    1215952756U,
    3201140055U,
};

/** One class's column of the Revelry markers. */
[[nodiscard]] constexpr std::array<std::uint32_t, kRevelryArmour.size()>
revelry_armour(std::uint32_t ClassArmourMarker::*piece) noexcept {
    std::array<std::uint32_t, kRevelryArmour.size()> column{};
    for (std::size_t index = 0; index < column.size(); ++index) {
        column[index] = kRevelryArmour[index].*piece;
    }
    return column;
}
constexpr auto kRevelryTitanArmour = revelry_armour(&ClassArmourMarker::titanArmour);
constexpr auto kRevelryHunterArmour = revelry_armour(&ClassArmourMarker::hunterArmour);
constexpr auto kRevelryWarlockArmour = revelry_armour(&ClassArmourMarker::warlockArmour);

constexpr std::array<SourcePool, 12> kSourcePools{{
    {GearSource::world, {kWorldEngram, {}, {}, {}}},
    {GearSource::blackArmoryArmour,
     {{}, kBlackArmoryTitanArmour, kBlackArmoryHunterArmour, kBlackArmoryWarlockArmour}},
    {GearSource::blackArmoryForge,
     {kBlackArmoryForgeWeapons,
      kBlackArmoryTitanArmour,
      kBlackArmoryHunterArmour,
      kBlackArmoryWarlockArmour}},
    {GearSource::revelry, {{}, kRevelryTitanArmour, kRevelryHunterArmour, kRevelryWarlockArmour}},
    {GearSource::dreamingCity,
     {kDreamingCityWeapons,
      kDreamingCityTitanArmour,
      kDreamingCityHunterArmour,
      kDreamingCityWarlockArmour}},
    {GearSource::gambit,
     {kGambitWeapons, kGambitTitanArmour, kGambitHunterArmour, kGambitWarlockArmour}},
    {GearSource::gambitPrimeWeapons, {kGambitPrimeWeapons, {}, {}, {}}},
    {GearSource::ironBanner,
     {kIronBannerWeapons,
      kIronBannerTitanArmour,
      kIronBannerHunterArmour,
      kIronBannerWarlockArmour}},
    {GearSource::lastWish,
     {kLastWishWeapons, kLastWishTitanArmour, kLastWishHunterArmour, kLastWishWarlockArmour}},
    {GearSource::scourge,
     {kScourgeWeapons, kScourgeTitanArmour, kScourgeHunterArmour, kScourgeWarlockArmour}},
    {GearSource::crown,
     {kCrownWeapons, kCrownTitanArmour, kCrownHunterArmour, kCrownWarlockArmour}},
    {GearSource::trials,
     {kTrialsWeapons, kTrialsTitanArmour, kTrialsHunterArmour, kTrialsWarlockArmour}},
}};

/** Werner's twelve weeklies and his introduction each pay one treasure map. */
constexpr std::array<TreasureMapTransition, 13> kTreasureMapTransitions{{
    {3042981914U, 561724901U},
    {3042981915U, 561724902U},
    {3042981912U, 561724896U},
    {3970388282U, 561724898U},
    {3970388283U, 561724899U},
    {3970388280U, 561724908U},
    {1262245655U, 2583850831U},
    {1262245654U, 2583850830U},
    {1262245653U, 2583850829U},
    {504411738U, 2583850828U},
    {504411739U, 2583850827U},
    {504411736U, 2583850825U},
    {2997169129U, 1492273683U},
}};

/** Werner's treasure maps. Only the world chest a map leads to completes it. */
[[nodiscard]] constexpr std::array<std::uint32_t, kTreasureMapTransitions.size()>
treasure_maps() noexcept {
    std::array<std::uint32_t, kTreasureMapTransitions.size()> maps{};
    for (std::size_t index = 0; index < maps.size(); ++index) {
        maps[index] = kTreasureMapTransitions[index].mapHash;
    }
    return maps;
}
constexpr auto kWernerTreasureMaps = treasure_maps();

/**
 * Plans the treasure map a Werner bounty pays in place of the rows it shows.
 * @param plan Receives the map's row and hash; unchanged for any other pursuit.
 * @return False when the bounty's map is not installed.
 */
[[nodiscard]] bool plan_treasure_map(std::uint32_t bountyHash, CompletionPlan& plan) noexcept {
    const TreasureMapTransition* transition = find_row(kTreasureMapTransitions, bountyHash);
    if (transition == nullptr) {
        return true;
    }
    build_data::items::Definition map{};
    if (!build_data::find_item_definition_hash(transition->mapHash, map)) {
        return false;
    }
    plan.rows[plan.rowCount++] = {map.definitionIndex, 1};
    plan.treasureMapHash = transition->mapHash;
    return true;
}

/**
 * Settles the treasure map a prepared grant placed for a Werner bounty.
 * @param deadline The deadline of the bounty that paid the map.
 * @return False when the map found no row, so the bounty would pay nothing.
 */
[[nodiscard]] bool settle_treasure_map(std::uint32_t mapHash,
                                       std::int32_t deadline,
                                       PendingRecordRewardGrant& mutation) noexcept {
    const PreparedRecordReward& placed = mutation.rewards[0];
    if (placed.placedQuantity == 0) {
        return false;
    }
    CharacterState& after = mutation.afterCharacter;
    const auto held = std::span(after.inventory.values).first(after.inventory.count);
    const auto map = std::find_if(held.begin(), held.end(), [&placed](const auto& item) {
        return item.instanceSoid != 0 && item.instanceSoid == placed.instanceSoid;
    });
    // A map keeps the deadline of the bounty that paid it.
    if (placed.definitionHash == mapHash && map != held.end() && map->objectives.has_value()) {
        (*map->objectives)[authored_inventory::kItemExpiryLane] = deadline;
    }
    return true;
}

/** Every bounty is in at most one family. */
constexpr std::array<BountyFamily, 19> kBountyFamilies{{
    {kRepeatableBounties, Cadence::repeatable, GearSource::world},
    {kDreamingCityDailyBounties, Cadence::daily, GearSource::dreamingCity},
    {kDreamingCityWeeklyBounties, Cadence::weekly, GearSource::dreamingCity},
    {kPurificationBounties, Cadence::none, GearSource::dreamingCity},
    {kTrialsDailyBounties, Cadence::daily, GearSource::world},
    {kTrialsWeeklyBounties, Cadence::weekly, GearSource::world},
    {kTrialsEndGameBounties, Cadence::weekly, GearSource::trials},
    {kHawthorneBounties, Cadence::weekly, GearSource::world},
    {kLunasRecallBounties, Cadence::weekly, GearSource::world},
    {kBlackArmoryArmourBounties, Cadence::weekly, GearSource::blackArmoryArmour},
    {kBlackArmoryForgeBounties, Cadence::weekly, GearSource::blackArmoryForge},
    {kRevelryBounties, Cadence::weekly, GearSource::revelry},
    {kIronBannerBounties, Cadence::weekly, GearSource::ironBanner},
    {kGambitBounties, Cadence::weekly, GearSource::gambit},
    {kGambitPrimeWeaponBounties, Cadence::weekly, GearSource::gambitPrimeWeapons},
    {kGambitPrimeRoleBounties, Cadence::weekly, GearSource::gambitPrimeRole},
    {kLastWishBounties, Cadence::weekly, GearSource::lastWish},
    {kScourgeBounties, Cadence::weekly, GearSource::scourge},
    {kCrownBounties, Cadence::weekly, GearSource::crown},
}};

// --- Reward rows ---------------------------------------------------------------------------

/** What one reward row pays. */
enum class PaymentKind : std::uint8_t {
    nothing,
    experience,
    rankPoints,
    roleHelmet,
    synthesizerUpgrade,
    item,
};

/** One reward row's payment, read through the authored tables. */
struct RowPayment {
    PaymentKind kind{PaymentKind::nothing};
    /** The multiple of its cadence's XP an experience row pays. */
    std::int32_t experienceMultiple{};
    /** The progression a rank-point row credits. */
    std::uint32_t progressionHash{};
    /** The item the row draws, which may be a wrapper. */
    std::uint16_t itemIndex{pursuit_data::kAbsent};
    /** The amount of that item, or the points a rank-point row credits. */
    std::int32_t quantity{};
    /** The role whose next helmet a role-helmet row pays, and the tier that helmet earns. */
    std::size_t role{};
    GambitPrimeHelmetTier helmetTier{};
    /** The held Synthesizer an upgrade row's item replaces. */
    std::uint64_t replacedSynthesizerSoid{};
};

/** How planning one reward row ended. */
enum class RowOutcome : std::uint8_t {
    planned,
    /** The row can never settle, so it pays nothing and the redemption goes on without it. */
    skipped,
    /** A later attempt may settle the row, so the redemption is refused. */
    refused,
};

/**
 * Reads a bounty's rules. Its family decides; a bounty no family names takes its cadence from its
 * lifetime, or else from a vendor's endless pool, and draws world gear.
 */
[[nodiscard]] BountyRules bounty_rules(const build_data::items::Definition& bounty,
                                       std::int32_t lifetimeSeconds) noexcept {
    BountyRules rules{};
    const auto family = std::find_if(
        kBountyFamilies.begin(), kBountyFamilies.end(), [&bounty](const BountyFamily& candidate) {
            return lists(candidate.bounties, bounty.definitionHash);
        });
    if (family != kBountyFamilies.end()) {
        rules = {family->cadence, family->gear};
    }
    if (rules.cadence != Cadence::none) {
        return rules;
    }
    if (lifetimeSeconds >= kWeeklyLifetimeFloorSeconds) {
        rules.cadence = Cadence::weekly;
    } else if (lifetimeSeconds > 0) {
        rules.cadence = Cadence::daily;
    } else if (build_data::in_repeatable_bounty_pool(bounty.definitionIndex)) {
        rules.cadence = Cadence::repeatable;
    }
    return rules;
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
 * Resolves the one item a gear marker pays from its bounty's source, or from world gear when the
 * source has no gear pool. A Revelry bounty pays the slot its Revelry marker names.
 * @return False when nothing the pool names is installed.
 */
[[nodiscard]] bool pay_gear(std::uint32_t markerHash,
                            GearSource source,
                            CharacterClass characterClass,
                            std::uint64_t seed,
                            std::uint16_t& itemIndex) noexcept {
    const ClassArmourMarker* slot = find_row(kRevelryArmour, markerHash);
    if (source == GearSource::revelry && slot != nullptr) {
        const std::array<std::uint32_t, 1> titan{slot->titanArmour};
        const std::array<std::uint32_t, 1> hunter{slot->hunterArmour};
        const std::array<std::uint32_t, 1> warlock{slot->warlockArmour};
        return draw_gear({{}, titan, hunter, warlock}, characterClass, seed, itemIndex);
    }
    const auto pool_of = [](GearSource wanted) noexcept {
        return std::find_if(
            kSourcePools.begin(), kSourcePools.end(), [wanted](const SourcePool& candidate) {
                return candidate.source == wanted;
            });
    };
    auto pool = pool_of(source);
    if (pool == kSourcePools.end()) {
        pool = pool_of(GearSource::world);
    }
    return pool != kSourcePools.end() && draw_gear(pool->pool, characterClass, seed, itemIndex);
}

/** @return True when the item is a gear marker, Revelry's included. */
[[nodiscard]] bool is_gear_marker(std::uint32_t hash) noexcept {
    return lists(kGearMarkers, hash) || find_row(kRevelryArmour, hash) != nullptr;
}

/**
 * Names the item a payment draws by its hash, and its amount.
 * @return False when the item is not installed.
 */
[[nodiscard]] bool
pay_item(std::uint32_t hash, std::int32_t quantity, RowPayment& payment) noexcept {
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_hash(hash, item)) {
        return false;
    }
    payment.itemIndex = item.definitionIndex;
    payment.quantity = quantity;
    return true;
}

/**
 * Reads a row naming an experience or rank-point marker, which pays no item and takes any
 * quantity. A rank-point row naming no points pays nothing.
 * @return False for any other row.
 */
[[nodiscard]] bool
pay_progress_marker(std::uint32_t hash, std::int32_t quantity, RowPayment& payment) noexcept {
    if (hash == kExperienceMarkerHash || hash == kDoubleExperienceMarkerHash) {
        payment.kind = PaymentKind::experience;
        payment.experienceMultiple = hash == kDoubleExperienceMarkerHash ? 2 : 1;
        return true;
    }
    const RankMarker* marker = find_row(kRankMarkers, hash);
    if (marker == nullptr) {
        return false;
    }
    if (quantity > 0) {
        payment.kind = PaymentKind::rankPoints;
        payment.progressionHash = marker->progressionHash;
        payment.quantity = quantity;
    }
    return true;
}

/**
 * Reads a row naming a currency. A row with no amount pays its bounty's cadence amount.
 * @param quantity The row's amount, zero or more.
 * @param reason Receives a static reason when the row is not settled.
 * @return Whether the amount and the currency settled, or nothing for any other row.
 */
[[nodiscard]] std::optional<bool> pay_currency(std::uint32_t hash,
                                               std::int32_t quantity,
                                               Cadence cadence,
                                               RowPayment& payment,
                                               const char*& reason) noexcept {
    const CurrencyPayout* payout = find_row(kCurrencyPayouts, hash);
    if (payout == nullptr) {
        return std::nullopt;
    }
    if (quantity == 0) {
        reason = "reward_quantity";
        if (cadence == Cadence::none) {
            return false;
        }
        quantity = cadence == Cadence::weekly ? payout->weeklyQuantity : payout->dailyQuantity;
    }
    reason = "reward_currency";
    payment.kind = PaymentKind::item;
    return pay_item(payout->currencyHash, quantity, payment);
}

/**
 * Reads a row naming a Gambit Prime role helmet or the Synthesizer Upgrade marker, which only a
 * role weekly pays.
 * @param reason Receives a static reason when the row is not settled.
 * @return Whether the bounty pays the marker, or nothing for any other row.
 */
[[nodiscard]] std::optional<bool> pay_role_marker(std::uint32_t hash,
                                                  std::int32_t quantity,
                                                  GearSource gear,
                                                  RowPayment& payment,
                                                  const char*& reason) noexcept {
    if (const RoleHelmetMarker* helmet = find_row(kRoleHelmets, hash)) {
        reason = "reward_role_helmet_source";
        payment.kind = PaymentKind::roleHelmet;
        payment.role = static_cast<std::size_t>(helmet - kRoleHelmets.data());
        return gear == GearSource::gambitPrimeRole;
    }
    if (hash == kSynthesizerUpgradeMarkerHash) {
        reason = "reward_synthesizer_upgrade_source";
        payment.kind = PaymentKind::synthesizerUpgrade;
        return gear == GearSource::gambitPrimeRole && quantity == 1;
    }
    return std::nullopt;
}

/**
 * Reads what one reward row pays. The authored tables decide a marker or currency they name; any
 * other row pays its own item, unless it names a stand-in.
 * @param seed Draws a gear marker's item.
 * @param reason Receives a static reason when the row is not settled.
 * @return False when the row names an item that is not installed or a stand-in no table pays, or
 *         the tables cannot settle its amount, gear or currency; it pays nothing.
 */
[[nodiscard]] bool row_payment(const pursuit_data::RewardPreview& reward,
                               const BountyRules& rules,
                               CharacterClass characterClass,
                               std::uint64_t seed,
                               RowPayment& payment,
                               const char*& reason) noexcept {
    payment = {};
    build_data::items::Definition item{};
    reason = "reward_item";
    if (!build_data::find_item_definition_index(reward.itemIndex, item)) {
        // No draw can pay an item that is not installed. A row naming no amount of one is empty.
        return reward.quantity == 0;
    }
    const std::uint32_t hash = item.definitionHash;
    if (pay_progress_marker(hash, reward.quantity, payment)) {
        return true;
    }
    reason = "reward_quantity";
    if (reward.quantity < 0) {
        return false;
    }
    if (const auto currency = pay_currency(hash, reward.quantity, rules.cadence, payment, reason)) {
        return *currency;
    }
    if (const auto role = pay_role_marker(hash, reward.quantity, rules.gear, payment, reason)) {
        return *role;
    }
    reason = "reward_gear_source";
    if (is_gear_marker(hash)) {
        payment.kind = PaymentKind::item;
        payment.quantity = 1;
        return pay_gear(hash, rules.gear, characterClass, seed, payment.itemIndex);
    }
    reason = "reward_rune_source";
    if (hash == kLegendaryRuneMarkerHash) {
        return false;
    }
    reason = "reward_stand_in";
    if (names_stand_in(reward.itemIndex)) {
        return false;
    }
    reason = "reward_quantity";
    if (reward.quantity == 0) {
        return false;
    }
    payment.kind = PaymentKind::item;
    payment.itemIndex = reward.itemIndex;
    payment.quantity = reward.quantity;
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
    progression_data::Definition definition{};
    std::array<progression_data::Step, progression_data::kStepPerDefinitionCapacity> steps{};
    std::size_t stepCount = 0;
    if (!build_data::find_progression_definition_hash(progressionHash, definition)
        || definition.scope == progression_data::Scope::unreplicated
        || !build_data::find_progression_steps(definition.definitionIndex, steps, stepCount)) {
        return false;
    }
    PreparedProgressionCredit& credit = plan.progression;
    if (credit.scope == progression_data::Scope::unreplicated) {
        credit = {definition.scope, definition.definitionIndex};
        credit.previousValue = progression_value(banks, credit);
        credit.value = credit.previousValue;
    } else if (credit.scope != definition.scope
               || credit.definitionIndex != definition.definitionIndex) {
        return false;
    }
    std::int64_t total = 0;
    for (const progression_data::Step& step : std::span(steps).first(stepCount)) {
        total += step.cost;
    }
    if (credit.value < total) {
        const std::int64_t credited = std::int64_t{credit.value} + quantity;
        credit.value = static_cast<std::int32_t>((std::min)(credited, total));
    }
    return true;
}

/** Logs a reward row that can never settle; the redemption skips it. */
void report_skipped_row(std::uint32_t bountyHash,
                        std::uint16_t rowItemIndex,
                        const char* reason) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=pursuit_redemption stage=reward_row result=skipped bounty=0x%08X "
                      "row_item=%u reason=%s",
                      static_cast<unsigned>(bountyHash),
                      static_cast<unsigned>(rowItemIndex),
                      reason);
}

/** Logs a reward row paid although its condition could not be judged. */
void report_unjudged_row(std::uint32_t bountyHash,
                         std::uint16_t rowItemIndex,
                         const char* reason) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=pursuit_redemption stage=reward_row result=unjudged_condition "
                      "bounty=0x%08X row_item=%u reason=%s",
                      static_cast<unsigned>(bountyHash),
                      static_cast<unsigned>(rowItemIndex),
                      reason);
}

/**
 * Names the helmet one tier past what the character has earned in the role, Notorious again once
 * that is earned, and the tier it earns.
 * @param plan Holds the character's helmet tiers, with the rows planned so far.
 * @return False when the helmet is not installed.
 */
[[nodiscard]] bool next_role_helmet(CharacterClass characterClass,
                                    const CompletionPlan& plan,
                                    RowPayment& payment) noexcept {
    const auto earned = static_cast<std::size_t>(plan.helmetTiers[payment.role]);
    const std::size_t next = (std::min)(earned + 1, kHelmetTierCount);
    const auto& tiers =
        kRoleHelmets[payment.role].classTiers[static_cast<std::size_t>(characterClass)];
    payment.helmetTier = static_cast<GambitPrimeHelmetTier>(next);
    return pay_item(tiers[next - 1], 1, payment);
}

/**
 * Names the Synthesizer one tier past the one the character holds, which it replaces. A Powerful
 * Synthesizer stays, and its row pays nothing.
 * @return False when the character holds no Synthesizer, or more than one, or the next tier is
 *         not installed.
 */
[[nodiscard]] bool next_synthesizer(const CharacterState& character, RowPayment& payment) noexcept {
    const std::size_t index = authored_inventory::held_synthesizer(character.inventory);
    if (index == character.inventory.count) {
        return false;
    }
    const authored_inventory::Item& held = character.inventory.values[index];
    const auto tier =
        static_cast<std::size_t>(authored_inventory::synthesizer_tier(held.definitionHash));
    if (tier == authored_inventory::kSynthesizerTiers.size()) {
        payment = {};
        return true;
    }
    payment.replacedSynthesizerSoid = held.instanceSoid;
    return held.instanceSoid != 0
           && pay_item(authored_inventory::kSynthesizerTiers[tier], 1, payment);
}

/**
 * Evaluates the condition one reward row names against the redeeming character's saved banks.
 * @param applies Receives whether the row pays; a row naming no condition always does.
 * @param reason Receives a static reason when the condition is not settled.
 * @return False when the condition did not read, bind or fit, or reads outside the saved banks.
 */
[[nodiscard]] bool row_applies(const pursuit_data::RewardPreview& reward,
                               const reward_resolution::Context& context,
                               bool& applies,
                               const char*& reason) noexcept {
    applies = false;
    if (reward.conditionUnreadable) {
        reason = "reward_condition_unreadable";
        return false;
    }
    const char* refusal = "reward_condition";
    const reward_resolution::Context conditionContext{
        context.unlocks, context.characterClass, context.seed, &refusal};
    if (!reward_resolution::eligible(
            std::span(reward.condition).first(reward.conditionCount), conditionContext, applies)) {
        reason = refusal;
        return false;
    }
    return true;
}

/**
 * Draws the item one row pays, which may be a wrapper, into the plan's grant rows. A role helmet's
 * tier and a replaced Synthesizer are kept only once their item is drawn.
 * @param context Saved banks and class of the redeeming character; its seed starts the draw.
 * @param reason Receives a static reason when the row is not planned.
 * @return Skipped when the draw is refused or outgrows the grant; refused when a catalog the draw
 *         reads was cleared meanwhile.
 */
[[nodiscard]] RowOutcome draw_row(const RowPayment& payment,
                                  const reward_resolution::Context& context,
                                  CompletionPlan& plan,
                                  const char*& reason) noexcept {
    const char* refusal = nullptr;
    const reward_resolution::Context drawContext{
        context.unlocks, context.characterClass, context.seed, &refusal};
    reward_resolution::Result resolved{};
    if (reward_resolution::resolve(
            drawContext, payment.itemIndex, static_cast<std::uint32_t>(payment.quantity), resolved)
        != reward_resolution::Resolution::resolved) {
        reason = refusal != nullptr ? refusal : "reward_draw";
        // A settled catalog's refusal is final; one cleared meanwhile may still pay the row.
        return catalogs_settled() ? RowOutcome::skipped : RowOutcome::refused;
    }
    reason = "reward_capacity";
    if (resolved.count > plan.grants.size() - plan.rowCount) {
        return RowOutcome::skipped;
    }
    plan.wrapperFlags |= resolved.wrapperFlags;
    const auto grants = std::span(resolved.grants).first(resolved.count);
    for (const reward_resolution::Grant& grant : grants) {
        plan.grants[plan.rowCount] = grant;
        const reward_resolution::Grant& kept = plan.grants[plan.rowCount];
        plan.rows[plan.rowCount++] = {
            kept.itemIndex, kept.quantity, std::span(kept.sockets).first(kept.socketCount), true};
    }
    if (payment.kind == PaymentKind::roleHelmet) {
        plan.helmetTiers[payment.role] = payment.helmetTier;
    }
    if (payment.kind == PaymentKind::synthesizerUpgrade) {
        plan.replacedSynthesizerSoid = payment.replacedSynthesizerSoid;
    }
    return RowOutcome::planned;
}

/**
 * Plans what one settled row pays: a rank credit, or an item drawn into the plan's grant rows.
 * @param character The redeeming character, whose held Synthesizer an upgrade replaces.
 * @param context Saved banks and class of the redeeming character; its seed starts the row's draw.
 * @param banks The same saved banks, which a progression credit reads its progress from.
 * @param reason Receives a static reason when the row is not planned.
 * @return Skipped when the row's progression, helmet or Synthesizer can never be paid.
 */
[[nodiscard]] RowOutcome pay_row(RowPayment payment,
                                 const CharacterState& character,
                                 const reward_resolution::Context& context,
                                 unlocks::Table& banks,
                                 CompletionPlan& plan,
                                 const char*& reason) noexcept {
    switch (payment.kind) {
    case PaymentKind::nothing:
    case PaymentKind::experience:
        return RowOutcome::planned;
    case PaymentKind::rankPoints:
        reason = "reward_rank_points";
        return plan_rank_points(payment.progressionHash, payment.quantity, banks, plan)
                   ? RowOutcome::planned
                   : RowOutcome::skipped;
    case PaymentKind::roleHelmet:
        reason = "reward_role_helmet";
        if (!next_role_helmet(context.characterClass, plan, payment)) {
            return RowOutcome::skipped;
        }
        break;
    case PaymentKind::synthesizerUpgrade:
        reason = "reward_synthesizer_upgrade";
        if (!next_synthesizer(character, payment)) {
            return RowOutcome::skipped;
        }
        if (payment.kind == PaymentKind::nothing) {
            return RowOutcome::planned;
        }
        break;
    case PaymentKind::item:
        break;
    }
    return draw_row(payment, context, plan, reason);
}

/**
 * Resolves the reward rows a pursuit shows into items, seasonal XP and a progression credit. A
 * wrapper draws from its own pool. A row whose condition does not hold pays nothing. A row whose
 * condition cannot be judged pays and is logged. A row that can never settle is logged and pays
 * nothing.
 * @param bountyHash The pursuit's item hash, which skipped rows are logged with.
 * @param rules The pursuit's cadence and gear source.
 * @param character The redeeming character, whose held Synthesizer an upgrade replaces.
 * @param context Saved banks and class of the redeeming character; its seed starts the draws. It
 *        carries no refusal slot: each condition and draw reports through a copy that has one.
 * @param banks The same saved banks, which a progression credit reads its progress from.
 * @param reason Receives a static reason when a row is skipped or refused.
 * @return False when a later attempt may still settle a row.
 */
[[nodiscard]] bool plan_rewards(std::uint32_t bountyHash,
                                const BountyRules& rules,
                                const CharacterState& character,
                                const pursuit_data::Definition& contract,
                                const reward_resolution::Context& context,
                                unlocks::Table& banks,
                                CompletionPlan& plan,
                                const char*& reason) noexcept {
    std::int32_t experienceMultiple = 0;
    bool upgradesSynthesizer = false;
    const auto rewards = std::span(contract.rewards).first(contract.rewardCount);
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        const pursuit_data::RewardPreview& reward = rewards[index];
        bool applies = false;
        if (!row_applies(reward, context, applies, reason)) {
            // TODO: source the computed values these conditions read. No saved bank holds them,
            // so the row pays as the pursuit shows it.
            report_unjudged_row(bountyHash, reward.itemIndex, reason);
            applies = true;
        }
        if (!applies) {
            continue;
        }
        const reward_resolution::Context rowContext{
            context.unlocks, context.characterClass, context.seed + index, context.refusal};
        RowPayment payment{};
        if (!row_payment(
                reward, rules, context.characterClass, rowContext.seed, payment, reason)) {
            report_skipped_row(bountyHash, reward.itemIndex, reason);
            continue;
        }
        if (payment.kind == PaymentKind::experience) {
            // Several experience rows describe one payout.
            experienceMultiple = (std::max)(experienceMultiple, payment.experienceMultiple);
            continue;
        }
        if (payment.kind == PaymentKind::synthesizerUpgrade) {
            // Repeated upgrade rows describe one upgrade.
            if (upgradesSynthesizer) {
                continue;
            }
            upgradesSynthesizer = true;
        }
        const RowOutcome outcome = pay_row(payment, character, rowContext, banks, plan, reason);
        if (outcome == RowOutcome::refused) {
            return false;
        }
        if (outcome == RowOutcome::skipped) {
            report_skipped_row(bountyHash, reward.itemIndex, reason);
        }
    }
    // A bounty with no cadence has no XP amount, so its experience rows pay nothing.
    if (experienceMultiple != 0 && rules.cadence == Cadence::none) {
        report_skipped_row(bountyHash, pursuit_data::kAbsent, "reward_experience_cadence");
    }
    plan.seasonalExperience = cadence_experience(rules.cadence) * experienceMultiple;
    return true;
}

// --- Redemption stages ---------------------------------------------------------------------

/** The held pursuit one redemption judges: its item, its installed contract and its own values. */
struct RedeemedPursuit {
    build_data::items::Definition item{};
    pursuit_data::Definition contract{};
    /** A copy that never saved any reads zero in every lane. */
    authored_inventory::ItemObjectives itemValues{};
};

/**
 * Finds the held pursuit a redemption names and checks that it can be redeemed at all.
 * @param reason Receives a static reason when the result is not prepared.
 * @return Refused when the request names no held, unlocked pursuit of that definition; not
 *         redeemable for an unreadable or suppressed contract, a treasure map or an expired bounty.
 */
[[nodiscard]] PursuitRedemption find_redeemed_pursuit(const CharacterState& character,
                                                      std::uint64_t instanceSoid,
                                                      std::uint32_t definitionHash,
                                                      std::int64_t nowSeconds,
                                                      RedeemedPursuit& pursuit,
                                                      const char*& reason) noexcept {
    const auto held = std::span(character.inventory.values).first(character.inventory.count);
    const auto source = std::find_if(held.begin(), held.end(), [instanceSoid](const auto& item) {
        return item.instanceSoid == instanceSoid;
    });
    reason = "pursuit";
    if (instanceSoid == 0 || source == held.end() || source->definitionHash != definitionHash
        || source->quantity != 1
        || !build_data::find_item_definition_hash(source->definitionHash, pursuit.item)
        || !build_data::find_pursuit_definition(pursuit.item.definitionIndex, pursuit.contract)) {
        return PursuitRedemption::refused;
    }
    reason = "pursuit_unreadable";
    if (!pursuit.contract.readable) {
        return PursuitRedemption::notRedeemable;
    }
    reason = "locked";
    if ((source->flags & authored_inventory::kLockedItemFlag) != 0) {
        return PursuitRedemption::refused;
    }
    reason = "suppressed";
    if (pursuit.contract.suppressCompletion) {
        return PursuitRedemption::notRedeemable;
    }
    reason = "treasure_map_world_chest";
    if (lists(kWernerTreasureMaps, pursuit.item.definitionHash)) {
        return PursuitRedemption::notRedeemable;
    }
    pursuit.itemValues = source->objectives.value_or(authored_inventory::ItemObjectives{});
    reason = "expired";
    if (bounty_expired(pursuit.contract, pursuit.itemValues, nowSeconds)) {
        return PursuitRedemption::notRedeemable;
    }
    return PursuitRedemption::prepared;
}

/**
 * Judges whether the server can read the pursuit's objectives, and reads them complete.
 * @param heldItems The character's held items, which a largest-value objective reads across.
 * @param reason Receives a static reason when the result is false.
 * @return False when the pursuit is not redeemed; it can still be removed.
 */
[[nodiscard]] bool objectives_complete(const RedeemedPursuit& pursuit,
                                       std::span<const authored_inventory::Item> heldItems,
                                       const reward_resolution::Context& context,
                                       const char*& reason) noexcept {
    const PursuitCompletionStatus status =
        inspect_pursuit_completion(pursuit.contract, context, pursuit.itemValues, heldItems);
    reason = "quest_progress";
    if (pursuit.contract.questStep.membership != pursuit_data::QuestMembership::none
        && !progress_item_held(status)) {
        return false;
    }
    // An objective on the deadline lane reads as unknown, so it is named before that check.
    reason = "objective_reads_deadline";
    if (reads_deadline(pursuit.contract)) {
        return false;
    }
    // Progress the server cannot read is progress it cannot call complete, so the pursuit is not
    // redeemed. It can still be removed, or it would hold its slot for good.
    reason = "objective_source";
    if (!status.objectivesKnown) {
        return false;
    }
    reason = "incomplete";
    return status.objectivesComplete;
}

/**
 * Plans setting the completion values the contract names.
 * @param reason Receives a static reason when the result is not prepared.
 * @return Refused when a named value maps to no saved row; not redeemable when the pursuit's
 *         completion value is already set.
 */
[[nodiscard]] PursuitRedemption plan_completion_values(const pursuit_data::Definition& contract,
                                                       unlocks::Table& banks,
                                                       CompletionPlan& plan,
                                                       const char*& reason) noexcept {
    reason = "completion_value";
    if (!plan_completion_value(contract.completionValue, banks, plan.completionValue)
        || !plan_completion_value(
            contract.completionContextValue, banks, plan.completionContextValue)) {
        return PursuitRedemption::refused;
    }
    reason = "completed";
    return plan.completionValue.previousValue != 0 ? PursuitRedemption::notRedeemable
                                                   : PursuitRedemption::prepared;
}

/**
 * Plans the quest step the pursuit moves to and what it pays: the next step's item, or at the
 * last step and outside any set, a Werner bounty's map or the reward rows the pursuit shows.
 * @param context Saved banks and class of the redeeming character; its seed starts the draws.
 * @param banks The same saved banks, which the quest step and a progression credit read.
 * @param reason Receives a static reason when the result is false.
 * @return False when the quest step, the map or a reward row cannot be planned now.
 */
[[nodiscard]] bool plan_payout(const RedeemedPursuit& pursuit,
                               const CharacterState& character,
                               const reward_resolution::Context& context,
                               unlocks::Table& banks,
                               CompletionPlan& plan,
                               const char*& reason) noexcept {
    const pursuit_data::QuestStep& step = pursuit.contract.questStep;
    reason = "quest_step";
    if (step.membership == pursuit_data::QuestMembership::unsupported
        || (step.membership == pursuit_data::QuestMembership::step
            && !plan_quest_step(step, banks, plan))) {
        return false;
    }
    if (step.membership != pursuit_data::QuestMembership::none
        && step.successorIndex != pursuit_data::kAbsent) {
        return true;
    }
    reason = "treasure_map";
    if (!plan_treasure_map(pursuit.item.definitionHash, plan)) {
        return false;
    }
    if (plan.treasureMapHash != authored_inventory::kNoDefinitionHash) {
        return true;
    }
    reason = "reward_rows";
    return pursuit.contract.rewardsReadable
           && plan_rewards(pursuit.item.definitionHash,
                           bounty_rules(pursuit.item, pursuit.contract.lifetimeSeconds),
                           character,
                           pursuit.contract,
                           context,
                           banks,
                           plan,
                           reason);
}

/**
 * Prepares the grant a plan describes, then adds the plan's saved effects to it.
 * @param instanceSoid The redeemed pursuit, which leaves unless it names a completion value.
 * @param reason Receives a static reason when the result is not prepared.
 * @return Not ready or refused as the grant's preparation ended; refused when a quest's next step
 *         or a Werner bounty's map found no row, which leaves no prepared grant behind.
 */
[[nodiscard]] PursuitRedemption prepare_planned_grant(const RedeemedPursuit& pursuit,
                                                      const CompletionPlan& plan,
                                                      std::uint64_t instanceSoid,
                                                      std::int64_t nowSeconds,
                                                      PendingRecordRewardGrant& mutation,
                                                      const char*& reason) noexcept {
    // What the redemption saves beside inventory rows makes it a grant even when no row is paid.
    const auto moves = [](const PreparedQuestStep& value) noexcept {
        return value.next.scope != Quest::Scope::none;
    };
    const bool savedEffects = moves(plan.completionValue) || moves(plan.completionContextValue)
                              || moves(plan.questStep)
                              || plan.progression.scope != progression_data::Scope::unreplicated
                              || plan.seasonalExperience != 0;
    // A pursuit naming a completion value keeps its item; the value records that it completed.
    const RewardGrantRequest request{
        .rewards = std::span(plan.rows).first(plan.rowCount),
        .wrapperFlags = &plan.wrapperFlags,
        .redeemedInstanceSoid = pursuit.contract.completionValue.declared ? 0 : instanceSoid,
        .replacedInstanceSoid = plan.replacedSynthesizerSoid,
        .nowSeconds = nowSeconds,
        .carriesSavedEffects = savedEffects};
    const RewardPreparation grant = prepare_reward_grant(request, mutation, &reason);
    if (grant != RewardPreparation::prepared) {
        return grant == RewardPreparation::notReady ? PursuitRedemption::notReady
                                                    : PursuitRedemption::refused;
    }
    // A step moved on without its item would leave the quest with no step to hold.
    reason = "quest_successor_placement";
    if (plan.questStep.next.scope != Quest::Scope::none
        && mutation.rewards[0].placedQuantity == 0) {
        mutation = {};
        return PursuitRedemption::refused;
    }
    // A bounty redeemed for a map that found no row would pay nothing.
    reason = "treasure_map_placement";
    if (plan.treasureMapHash != authored_inventory::kNoDefinitionHash
        && !settle_treasure_map(plan.treasureMapHash,
                                pursuit.itemValues[authored_inventory::kItemExpiryLane],
                                mutation)) {
        mutation = {};
        return PursuitRedemption::refused;
    }
    mutation.completionValue = plan.completionValue;
    mutation.completionContextValue = plan.completionContextValue;
    mutation.afterCharacter.gambitPrimeHelmetTiers = plan.helmetTiers;
    mutation.questStep = plan.questStep;
    mutation.progression = plan.progression;
    mutation.seasonalExperience = plan.seasonalExperience;
    mutation.seasonalExperienceBefore = seasonal_experience();
    reason = nullptr;
    return PursuitRedemption::prepared;
}

} // namespace

namespace runtime::detail {

/** The contracts, the reward graph, the progressions and the repeatable pool. */
bool pursuit_redemption_catalogs_ready() noexcept {
    return build_data::pursuit_definitions_ready() && build_data::reward_definitions_ready()
           && build_data::progression_definitions_ready()
           && build_data::repeatable_bounties_ready();
}

/** Each stage ends the redemption with its own outcome and reason, in the order a request reads. */
PursuitRedemption plan_pursuit_redemption(SavedSelection<AccountState>& saved,
                                          std::uint64_t instanceSoid,
                                          std::uint32_t definitionHash,
                                          std::int64_t nowSeconds,
                                          std::uint64_t seed,
                                          PendingRecordRewardGrant& mutation,
                                          const char*& reason) noexcept {
    mutation = {};
    const CharacterState& character = saved.account.characters[saved.slot];
    RedeemedPursuit pursuit{};
    const PursuitRedemption found =
        find_redeemed_pursuit(character, instanceSoid, definitionHash, nowSeconds, pursuit, reason);
    if (found != PursuitRedemption::prepared) {
        return found;
    }
    // No refusal slot: a reward row's condition or draw reports through a copy that has one.
    const reward_resolution::Context context{
        saved.banks, character.characterClass, seed, nullptr};
    const auto held = std::span(character.inventory.values).first(character.inventory.count);
    if (!objectives_complete(pursuit, held, context, reason)) {
        return PursuitRedemption::notRedeemable;
    }
    CompletionPlan plan{};
    plan.helmetTiers = character.gambitPrimeHelmetTiers;
    const PursuitRedemption values =
        plan_completion_values(pursuit.contract, saved.banks, plan, reason);
    if (values != PursuitRedemption::prepared) {
        return values;
    }
    if (!plan_payout(pursuit, character, context, saved.banks, plan, reason)) {
        return PursuitRedemption::refused;
    }
    return prepare_planned_grant(pursuit, plan, instanceSoid, nowSeconds, mutation, reason);
}

} // namespace runtime::detail

/** Reads one saved view under the store lock, then judges and plans against it. */
PursuitRedemption prepare_pursuit_redemption(std::uint64_t instanceSoid,
                                             std::uint32_t definitionHash,
                                             std::int64_t nowSeconds,
                                             std::uint64_t seed,
                                             PendingRecordRewardGrant& mutation,
                                             const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    mutation = {};
    reason = "not_ready";
    if (!pursuit_redemption_catalogs_ready()) {
        return PursuitRedemption::notReady;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    const std::unique_ptr<SavedSelection<AccountState>> saved(
        new (std::nothrow) SavedSelection<AccountState>{});
    reason = "saved_state";
    if (!saved || read_saved_selection(*saved) != SavedSelectionRead::read) {
        return PursuitRedemption::notReady;
    }
    return plan_pursuit_redemption(
        *saved, instanceSoid, definitionHash, nowSeconds, seed, mutation, reason);
}

} // namespace sunrise::state
