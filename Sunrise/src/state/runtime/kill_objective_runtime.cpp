/**
 * Kill objectives: a kill the selected character's player made advances the per-item objective
 * lanes of the held bounties it meets. The victim's race and the killing source's weapon, ability
 * and precision come from the installed combat taxonomy; the destination, slot and damage type
 * come from the incident.
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string_view>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace objectives = build_data::objectives;
namespace pursuits = build_data::pursuits;

namespace {

// The label table names its labels and groups; these are FNV-1 hashes of those names.
/** Group holding every player label. A player-attributed source carries exactly one. */
constexpr std::uint32_t kPlayerGroup = 0x3FBE3C2AU; // "player"
/** Group holding every combatant label, which a player-attributed source never carries. */
constexpr std::uint32_t kCombatantGroup = 0x905E8440U; // "combatant"
/** Group holding every weapon label; a weapon kill's source carries exactly one. */
constexpr std::uint32_t kWeaponGroup = 0xE810D505U; // "weapon"
constexpr std::uint32_t kMeleeGroup = 0xBF39E12BU;   // "melee"
constexpr std::uint32_t kGrenadeGroup = 0xC20DD425U; // "grenade"
constexpr std::uint32_t kSuperGroup = 0xB9D34B96U;   // "super"
constexpr std::uint32_t kAbilityGroup = 0x485FC991U; // "ability"
/** Label a precision final blow adds to its source. */
constexpr std::uint32_t kPrecisionLabel = 0x962EA19BU; // "precision"
/** Victim label of Scorn, which the race table has no row for. */
constexpr std::uint32_t kScornLabel = 0x512D8020U; // "forsaken"
/** The victim rank labels; a victim carries at most one. */
constexpr std::uint32_t kBaseLabel = 0x4CF9B596U;     // "base"
constexpr std::uint32_t kBossLabel = 0x5CFE4C22U;     // "boss"
constexpr std::uint32_t kEliteLabel = 0x574459CEU;    // "elite"
constexpr std::uint32_t kMegabossLabel = 0x04F136E0U; // "megaboss"
constexpr std::uint32_t kMinibossLabel = 0x9A3D2CCBU; // "miniboss"
constexpr std::array<std::uint32_t, 5> kRankLabels{
    kBaseLabel, kBossLabel, kEliteLabel, kMegabossLabel, kMinibossLabel};

/** Weapon labels the objectives name. */
constexpr std::uint32_t kAutoRifle = 0xDEE598FEU;       // "auto rifle"
constexpr std::uint32_t kBow = 0x2099B90FU;             // "bow"
constexpr std::uint32_t kGrenadeLauncher = 0xC736F413U; // "grenade launcher"
constexpr std::uint32_t kHandCannon = 0xDA448BC9U;      // "hand cannon"
constexpr std::uint32_t kPulseRifle = 0x937FE7FAU;      // "pulse rifle"
constexpr std::uint32_t kRocketLauncher = 0x0BB32533U;  // "rocket launcher"
constexpr std::uint32_t kScoutRifle = 0xFA4C7ED3U;      // "scout rifle"
constexpr std::uint32_t kShotgun = 0x0314A289U;         // "shotgun"
constexpr std::uint32_t kSniperRifle = 0x452E6656U;     // "sniper rifle"
constexpr std::uint32_t kSubmachineGun = 0xE5608E0EU;   // "submachine gun"
constexpr std::uint32_t kSword = 0x924E78C4U;           // "sword"

/** Source kinds of a final blow, as bits; a rule requires every bit it names. */
constexpr std::uint8_t kWeapon = 1;
constexpr std::uint8_t kGrenade = 2;
constexpr std::uint8_t kSuper = 4;
constexpr std::uint8_t kMelee = 8;
/** An ability-group source that carries no weapon label. */
constexpr std::uint8_t kAbility = 16;
constexpr std::uint8_t kPrecision = 32;

// --- Authored game data --------------------------------------------------------------------

// TODO: derive these from the objectives' own conditions; no extractor reads which kills an
// objective counts, so each mapped objective is named here.

/** Ordinals of the installed race enumeration; Scorn has no row and comes from its label. */
enum class Race : std::uint8_t {
    cabal = 0,
    fallen = 1,
    hive = 2,
    vex = 3,
    taken = 6,
    scorn = 0xFE,
    any = 0xFF,
};

/** Final-blow damage types, as the incident encodes them once unbiased. */
enum class Element : std::int8_t {
    any = -1,
    kinetic = 0,
    solar = 1,
    arc = 2,
    voidDamage = 3,
};

/** Equipment slots of the three weapon slots, as the incident names the killing source's. */
enum class WeaponSlot : std::int8_t {
    any = -1,
    kinetic = 7,
    energy = 8,
    heavy = 9,
};

/** Free-roam destinations the objectives name, by the package their activity loads. */
constexpr std::string_view kDreamingCity = "dreaming_city_freeroam";
constexpr std::string_view kEdz = "edz_freeroam";
constexpr std::string_view kIo = "eden_freeroam";
constexpr std::string_view kMars = "polaris_freeroam";
constexpr std::string_view kMercury = "mercury_freeroam";
constexpr std::string_view kMoon = "luna_freeroam";
constexpr std::string_view kNessus = "planet_x_freeroam";
constexpr std::string_view kTangledShore = "tangled_shore_freeroam";
constexpr std::string_view kTitan = "fleet_freeroam";

/** Sources one of which must match; a rule naming none places no condition here. */
struct Alternatives {
    std::array<std::uint32_t, 4> weaponLabels{};
    std::uint8_t sources{};
    WeaponSlot slot{WeaponSlot::any};
};

/** One held bounty objective a kill advances, and what the kill must show. */
struct KillRule {
    std::uint32_t itemHash{};
    /** Objective position in the bounty's installed contract. */
    std::uint8_t objective{};
    /** Destination package the kill must happen in; empty for any. */
    std::string_view destination{};
    Race race{Race::any};
    /** Weapon label the final blow must come from; zero for any. */
    std::uint32_t weaponLabel{};
    Element element{Element::any};
    /** Every source kind the final blow must show. */
    std::uint8_t sources{};
    WeaponSlot slot{WeaponSlot::any};
    /** Rank label the victim must carry; zero for any. */
    std::uint32_t rankLabel{};
    /** Actor class the victim must be; zero for any. */
    std::uint32_t victimClass{};
    Alternatives anyOf{};
};

/** One row per bounty objective lane; each kill advances a lane by one. */
constexpr auto kKillRules = std::to_array<KillRule>({
    {.itemHash = 0x7296E6D8U, .race = Race::fallen},
    {.itemHash = 0x7296E6DAU, .race = Race::cabal},
    {.itemHash = 0x7296E6DCU, .race = Race::taken},
    {.itemHash = 0xFAFC79E1U, .destination = kIo, .race = Race::taken},
    {.itemHash = 0x08CF365AU, .destination = kEdz},
    {.itemHash = 0xEBBB4B67U, .destination = kEdz, .race = Race::fallen},
    {.itemHash = 0xF78AFDC6U, .destination = kEdz, .race = Race::cabal},
    {.itemHash = 0xBCE7E301U, .destination = kTitan},
    {.itemHash = 0xEBEDB6B5U, .destination = kNessus},
    {.itemHash = 0x20FD444BU, .destination = kNessus, .race = Race::fallen},
    {.itemHash = 0x4F435712U, .destination = kNessus, .race = Race::cabal},
    {.itemHash = 0x4CAEC586U, .destination = kMercury},
    {.itemHash = 0xD344C544U, .destination = kMars},
    {.itemHash = 0xBF8448F4U, .destination = kMars, .race = Race::cabal},
    {.itemHash = 0xC8E05C58U, .destination = kTangledShore},
    {.itemHash = 0x80BD0E33U, .destination = kIo, .race = Race::taken},
    {.itemHash = 0x28665292U, .race = Race::cabal},
    {.itemHash = 0x321406E3U, .race = Race::fallen},
    {.itemHash = 0x321406E2U, .race = Race::taken},
    {.itemHash = 0x7296E6DFU, .weaponLabel = kSword},
    {.itemHash = 0x7296E6DEU, .weaponLabel = kAutoRifle},
    {.itemHash = 0x9507AC48U, .destination = kEdz, .weaponLabel = kSniperRifle},
    {.itemHash = 0xB9550F2BU, .destination = kMoon, .weaponLabel = kAutoRifle},
    {.itemHash = 0xB9550F2AU, .destination = kMoon, .weaponLabel = kSubmachineGun},
    {.itemHash = 0xB9550F24U, .destination = kMoon, .weaponLabel = kHandCannon},
    {.itemHash = 0x3B8D663AU, .element = Element::voidDamage},
    {.itemHash = 0x3B8D663BU, .element = Element::solar},
    {.itemHash = 0x6DEAB792U, .destination = kIo, .element = Element::voidDamage},
    {.itemHash = 0x8E5D7083U, .destination = kIo, .element = Element::solar},
    {.itemHash = 0xF5BB6A03U, .destination = kEdz, .element = Element::voidDamage},
    {.itemHash = 0x9055E0F2U, .destination = kTitan, .element = Element::voidDamage},
    {.itemHash = 0x6C382BE3U, .destination = kTitan, .element = Element::solar},
    {.itemHash = 0xB9550F2CU, .destination = kMoon, .element = Element::solar},
    {.itemHash = 0xB9550F2FU, .destination = kMoon, .element = Element::voidDamage},
    {.itemHash = 0x3B8D663CU, .element = Element::arc},
    {.itemHash = 0x5CBBBE3CU, .destination = kIo, .element = Element::arc},
    {.itemHash = 0x8DF2981FU, .destination = kEdz, .element = Element::arc},
    {.itemHash = 0xD72C739CU, .destination = kTitan, .element = Element::arc},
    {.itemHash = 0xB9550F2DU, .destination = kMoon, .element = Element::arc},
    {.itemHash = 0x7296E6D1U, .sources = kPrecision},
    {.itemHash = 0x3B8D6631U, .sources = kSuper},
    {.itemHash = 0x28665299U, .sources = kGrenade},
    {.itemHash = 0x772B07C1U, .destination = kIo, .sources = kSuper},
    {.itemHash = 0xC506DF36U, .destination = kIo, .sources = kGrenade},
    {.itemHash = 0x23C1FAD4U, .destination = kIo, .sources = kPrecision},
    {.itemHash = 0x28DFBC86U, .destination = kEdz, .sources = kSuper},
    {.itemHash = 0x321D9F09U, .destination = kEdz, .sources = kGrenade},
    {.itemHash = 0x740942DBU, .destination = kEdz, .sources = kPrecision},
    {.itemHash = 0xFC4D8521U, .destination = kTitan, .sources = kSuper},
    {.itemHash = 0x8A48A696U, .destination = kTitan, .sources = kGrenade},
    {.itemHash = 0xDD1D7234U, .destination = kTitan, .sources = kPrecision},
    {.itemHash = 0xE9BE1ECDU, .destination = kNessus, .sources = kSuper},
    {.itemHash = 0x1A1BC6CAU, .destination = kNessus, .sources = kGrenade},
    {.itemHash = 0x1D076088U, .destination = kNessus, .sources = kPrecision},
    {.itemHash = 0xB9550F2EU, .destination = kMoon, .sources = kGrenade},
    {.itemHash = 0x4B3C5716U, .sources = kPrecision},
    {.itemHash = 0x3069FA4BU, .destination = kMars, .sources = kPrecision},
    {.itemHash = 0x7296E6D9U, .race = Race::hive},
    {.itemHash = 0x7296E6DDU, .race = Race::vex},
    {.itemHash = 0x3B8D6638U, .anyOf = {.weaponLabels = {kRocketLauncher}, .sources = kGrenade}},
    {.itemHash = 0x3B8D663EU,
     .anyOf = {.weaponLabels = {kScoutRifle, kSniperRifle, kPulseRifle, kBow}}},
    {.itemHash = 0x2E47D6B9U, .destination = kIo, .race = Race::vex},
    {.itemHash = 0xAAA306EBU, .destination = kTitan, .race = Race::hive},
    {.itemHash = 0x9A412E35U, .destination = kNessus, .weaponLabel = kSniperRifle},
    {.itemHash = 0x53FCD57EU, .destination = kNessus, .element = Element::voidDamage},
    {.itemHash = 0x55623448U, .destination = kNessus, .element = Element::arc},
    {.itemHash = 0x2D16CE2FU, .destination = kNessus, .element = Element::solar},
    {.itemHash = 0xC10FA21AU, .destination = kMercury, .sources = kSuper},
    {.itemHash = 0x52711B35U, .destination = kMercury, .sources = kGrenade},
    {.itemHash = 0xD019F9CDU, .race = Race::vex, .sources = kPrecision},
    {.itemHash = 0x8D419E87U, .destination = kMercury, .element = Element::voidDamage},
    {.itemHash = 0x6F16E7E3U, .destination = kMercury, .element = Element::arc},
    {.itemHash = 0x3BDD219CU, .destination = kMars, .element = Element::solar},
    {.itemHash = 0xC170B55BU, .destination = kMars, .sources = kGrenade},
    {.itemHash = 0x63D3CE9DU, .destination = kMars, .race = Race::hive},
    {.itemHash = 0x34D450B5U, .destination = kMars, .element = Element::arc},
    {.itemHash = 0xEC226745U, .destination = kMars, .element = Element::voidDamage},
    {.itemHash = 0xC8E05C5BU, .destination = kTangledShore, .sources = kPrecision},
    {.itemHash = 0xC8E05C5CU, .destination = kTangledShore, .sources = kSuper},
    {.itemHash = 0x2233C579U, .anyOf = {.weaponLabels = {kSword, kSniperRifle, kBow}}},
    {.itemHash = 0x2233C578U, .anyOf = {.weaponLabels = {kBow, kSubmachineGun, kHandCannon}}},
    {.itemHash = 0x80BD0E3DU, .destination = kMars, .race = Race::hive},
    {.itemHash = 0x80BD0E3CU, .destination = kMercury, .race = Race::vex},
    {.itemHash = 0x2866529AU, .element = Element::solar, .sources = kWeapon},
    {.itemHash = 0x3314085BU, .race = Race::hive},
    {.itemHash = 0x321406E8U, .race = Race::vex},
    {.itemHash = 0x0ECA9E33U, .destination = kTitan, .sources = kPrecision},
    {.itemHash = 0xE71C88DBU, .destination = kMercury, .race = Race::vex},
    {.itemHash = 0xE71C88DFU, .destination = kMercury, .sources = kPrecision},
    {.itemHash = 0x7296E6D0U, .sources = kMelee},
    {.itemHash = 0x4A71C838U, .destination = kIo, .sources = kMelee},
    {.itemHash = 0x93A24325U, .destination = kMars, .sources = kMelee},
    {.itemHash = 0xB9550F29U, .destination = kMoon, .sources = kMelee},
    {.itemHash = 0x7296E6DBU, .race = Race::scorn},
    {.itemHash = 0x2866529BU, .race = Race::scorn},
    {.itemHash = 0x8DF57AF7U, .element = Element::arc, .sources = kWeapon},
    {.itemHash = 0x14F1A79AU, .element = Element::solar, .sources = kWeapon},
    {.itemHash = 0x5D76391DU, .element = Element::voidDamage, .sources = kWeapon},
    {.itemHash = 0xE28EF43EU, .race = Race::taken},
    {.itemHash = 0xE28EF43DU, .race = Race::fallen},
    {.itemHash = 0xE28EF43CU, .race = Race::cabal},
    {.itemHash = 0xE28EF433U, .race = Race::hive},
    {.itemHash = 0xB5603818U, .race = Race::cabal},
    {.itemHash = 0xB5603818U, .objective = 1, .race = Race::vex},
    {.itemHash = 0xB5603818U, .objective = 2, .race = Race::hive},
    {.itemHash = 0xB5603818U, .objective = 3, .race = Race::taken},
    {.itemHash = 0xB5603818U, .objective = 4, .race = Race::scorn},
    {.itemHash = 0xB5603818U, .objective = 5, .race = Race::fallen},
    {.itemHash = 0x321406EAU, .rankLabel = kBossLabel},
    {.itemHash = 0xE71C88DCU, .destination = kMercury, .rankLabel = kBossLabel},
    {.itemHash = 0x0337775BU, .destination = kMars, .rankLabel = kBossLabel},
    {.itemHash = 0xE68B6181U, .destination = kMercury, .element = Element::solar,
     .sources = kAbility},
    {.itemHash = 0x321406ECU, .element = Element::voidDamage, .sources = kAbility},
    {.itemHash = 0x82640576U, .destination = kIo, .sources = kAbility},
    {.itemHash = 0x0ECA9E35U, .destination = kTitan, .element = Element::arc, .sources = kAbility},
    {.itemHash = 0xE71C88D8U, .destination = kMercury, .element = Element::solar,
     .sources = kAbility},
    {.itemHash = 0x0337775DU, .destination = kMars, .element = Element::voidDamage,
     .sources = kAbility},
    {.itemHash = 0x12DB6D9BU, .sources = kMelee},
    {.itemHash = 0x12DB6D98U, .element = Element::solar},
    {.itemHash = 0x12DB6D9EU, .weaponLabel = kBow},
    {.itemHash = 0x27F60EBFU, .rankLabel = kEliteLabel, .anyOf = {.sources = kPrecision | kAbility}},
    {.itemHash = 0x3314085DU, .weaponLabel = kGrenadeLauncher, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x82640577U, .destination = kIo, .sources = kWeapon, .slot = WeaponSlot::kinetic},
    {.itemHash = 0x82640577U, .objective = 1, .destination = kIo, .sources = kWeapon,
     .slot = WeaponSlot::energy},
    {.itemHash = 0x82640577U, .objective = 2, .destination = kIo, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x8DF57AF1U, .element = Element::arc, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x14F1A79CU, .element = Element::solar, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x5D76391BU, .element = Element::voidDamage, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0xE39D657BU, .sources = kWeapon, .slot = WeaponSlot::kinetic},
    {.itemHash = 0xE39D657BU, .objective = 1, .sources = kWeapon, .slot = WeaponSlot::energy},
    {.itemHash = 0xE39D657BU, .objective = 2, .sources = kWeapon, .slot = WeaponSlot::heavy},
    {.itemHash = 0x0ECA9E36U, .destination = kTitan, .sources = kWeapon, .slot = WeaponSlot::energy},
    {.itemHash = 0x77F6C3D4U, .destination = kMercury, .race = Race::vex,
     .victimClass = 0xCF060C30U},
    {.itemHash = 0xA6D6C818U, .destination = kTitan, .sources = kMelee | kAbility},
    {.itemHash = 0x2233C57AU, .anyOf = {.weaponLabels = {kShotgun}, .slot = WeaponSlot::heavy}},
    {.itemHash = 0x9B3F41E5U, .destination = kDreamingCity, .race = Race::taken,
     .rankLabel = kEliteLabel},
});

// --- Kill matching -------------------------------------------------------------------------

/** The facts of one enemy kill a rule is checked against. */
struct KillFacts {
    std::string_view destination{};
    Race race{Race::any};
    std::uint32_t victimClass{};
    std::uint32_t rankLabel{};
    std::uint8_t sources{};
    std::uint32_t weaponLabel{};
    WeaponSlot slot{WeaponSlot::any};
    Element element{Element::any};
};

/** @return The labels in the named group, and the last of them in `member`. */
[[nodiscard]] std::size_t
members(std::span<const std::uint32_t> labels, std::uint32_t group, std::uint32_t& member) noexcept {
    std::size_t count = 0;
    for (const std::uint32_t label : labels) {
        if (build_data::label_in_group(group, label)) {
            member = label;
            ++count;
        }
    }
    return count;
}

/** @return The labels in the named group. */
[[nodiscard]] std::size_t members(std::span<const std::uint32_t> labels,
                                  std::uint32_t group) noexcept {
    std::uint32_t unused = 0;
    return members(labels, group, unused);
}

/** @return True when the list carries the label. */
[[nodiscard]] bool carries(std::span<const std::uint32_t> labels, std::uint32_t label) noexcept {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

/** @return True for the race ordinals of enemy races; the rest name players and treasure. */
[[nodiscard]] bool enemy_race(Race race) noexcept {
    return race == Race::cabal || race == Race::fallen || race == Race::hive || race == Race::vex
           || race == Race::taken;
}

/**
 * Reads the victim's race, rank and class.
 * @return False when the victim is not a combatant of a known enemy race.
 */
[[nodiscard]] bool classify_victim(const KillObservation& kill, KillFacts& facts) noexcept {
    if (kill.victimKey != 0 || !kill.victimCombatant || kill.victimClassHash == 0
        || members(kill.victimLabels, kPlayerGroup) != 0) {
        return false;
    }
    facts.victimClass = kill.victimClassHash;
    std::uint8_t ordinal = 0;
    const bool installed = build_data::find_enemy_race(kill.victimClassHash, ordinal);
    if (carries(kill.victimLabels, kScornLabel)) {
        // A class the race table places elsewhere contradicts the label, so neither is trusted.
        if (installed) {
            return false;
        }
        facts.race = Race::scorn;
    } else if (!installed || !enemy_race(static_cast<Race>(ordinal))) {
        return false;
    } else {
        facts.race = static_cast<Race>(ordinal);
    }
    std::size_t ranks = 0;
    for (const std::uint32_t label : kRankLabels) {
        if (carries(kill.victimLabels, label)) {
            facts.rankLabel = label;
            ++ranks;
        }
    }
    if (ranks != 1) {
        facts.rankLabel = 0;
    }
    return true;
}

/**
 * Reads the killing source's kinds, weapon label and slot. A source not attributed to one player,
 * or showing no known kind, leaves them empty: only rules naming no source can still match.
 */
void classify_source(const KillObservation& kill, KillFacts& facts) noexcept {
    std::uint32_t player = 0;
    std::uint32_t actor = 0;
    if (members(kill.sourceLabels, kPlayerGroup, player) != 1
        || members(kill.actorLabels, kPlayerGroup, actor) != 1 || player != actor
        || members(kill.sourceLabels, kCombatantGroup) != 0) {
        return;
    }
    std::uint32_t weapon = 0;
    const std::size_t weapons = members(kill.sourceLabels, kWeaponGroup, weapon);
    const bool melee = members(kill.sourceLabels, kMeleeGroup) != 0;
    const bool grenade = members(kill.sourceLabels, kGrenadeGroup) != 0;
    const bool super = members(kill.sourceLabels, kSuperGroup) != 0;
    const bool ability = members(kill.sourceLabels, kAbilityGroup) != 0;
    std::uint8_t sources = 0;
    if (melee) {
        sources |= kMelee;
    }
    if (grenade) {
        sources |= kGrenade;
    }
    if (super) {
        sources |= kSuper;
    }
    if (ability && weapons == 0) {
        sources |= kAbility;
    }
    if (weapons == 1 && !ability && !melee && !grenade && !super) {
        sources |= kWeapon;
        facts.weaponLabel = weapon;
        // The slot names the killing weapon only when no ability fired it.
        if (kill.abilitySelector == -1 && kill.equipmentSlot.has_value()) {
            const auto slot = static_cast<WeaponSlot>(*kill.equipmentSlot);
            if (slot == WeaponSlot::kinetic || slot == WeaponSlot::energy
                || slot == WeaponSlot::heavy) {
                facts.slot = slot;
            }
        }
    }
    if (sources == 0) {
        return;
    }
    if (carries(kill.sourceLabels, kPrecisionLabel)) {
        sources |= kPrecision;
    }
    facts.sources = sources;
    const auto element = static_cast<Element>(kill.damageType);
    if (element == Element::kinetic || element == Element::solar || element == Element::arc
        || element == Element::voidDamage) {
        facts.element = element;
    }
}

/** @return True when no alternative is named, or one of the named ones matches. */
[[nodiscard]] bool alternative_matches(const Alternatives& anyOf, const KillFacts& kill) noexcept {
    const auto& labels = anyOf.weaponLabels;
    const bool named = anyOf.sources != 0 || anyOf.slot != WeaponSlot::any
                       || std::any_of(labels.begin(), labels.end(), [](std::uint32_t label) {
                              return label != 0;
                          });
    if (!named) {
        return true;
    }
    const bool weaponMatches =
        (kill.sources & kWeapon) != 0
        && std::find(labels.begin(), labels.end(), kill.weaponLabel) != labels.end();
    return weaponMatches || (anyOf.sources & kill.sources) != 0
           || (anyOf.slot != WeaponSlot::any && anyOf.slot == kill.slot);
}

/** @return True when the kill meets every condition the rule names. */
[[nodiscard]] bool matches(const KillRule& rule, const KillFacts& kill) noexcept {
    return (rule.destination.empty() || rule.destination == kill.destination)
           && (rule.race == Race::any || rule.race == kill.race)
           && (rule.weaponLabel == 0
               || ((kill.sources & kWeapon) != 0 && rule.weaponLabel == kill.weaponLabel))
           && (rule.element == Element::any || rule.element == kill.element)
           && (rule.sources & kill.sources) == rule.sources
           && (rule.slot == WeaponSlot::any || rule.slot == kill.slot)
           && (rule.rankLabel == 0 || rule.rankLabel == kill.rankLabel)
           && (rule.victimClass == 0 || rule.victimClass == kill.victimClass)
           && alternative_matches(rule.anyOf, kill);
}

/**
 * Finds the item lane one objective's progress lives in, and its target.
 * @return False when the objective's progress is not one of the item's own values, or its target
 *         is not a plain count up to its completion value.
 */
[[nodiscard]] bool objective_lane(const pursuits::Definition& contract,
                                  std::uint8_t ordinal,
                                  std::size_t& lane,
                                  std::int32_t& target) noexcept {
    objectives::Definition objective{};
    if (ordinal >= contract.objectiveCount
        || !build_data::find_objective_definition(contract.objectives[ordinal], objective)
        || objective.countsDown || !objective.targetAdjustment.empty()
        || objective.itemValueSlot == objectives::kNoItemValueSlot
        || objective.completionValue <= 0) {
        return false;
    }
    const auto slots = std::span(contract.itemValueSlots).first(contract.itemValueSlotCount);
    const auto found = std::find(slots.begin(), slots.end(), objective.itemValueSlot);
    lane = static_cast<std::size_t>(found - slots.begin());
    target = objective.completionValue;
    return found != slots.end() && lane < authored_inventory::kItemObjectiveLaneCount;
}

/**
 * Advances every open lane of one held item that the kill meets.
 * @return The lanes advanced.
 */
[[nodiscard]] std::size_t advance_item(authored_inventory::Item& held,
                                       const KillFacts& kill,
                                       std::int64_t nowSeconds) noexcept {
    const auto rules = std::span(kKillRules);
    const bool mapped = std::any_of(rules.begin(), rules.end(), [&](const KillRule& rule) {
        return rule.itemHash == held.definitionHash;
    });
    build_data::items::Definition item{};
    pursuits::Definition contract{};
    if (!mapped || !build_data::find_item_definition_hash(held.definitionHash, item)
        || !build_data::find_pursuit_definition(item.definitionIndex, contract)
        || !contract.readable) {
        return 0;
    }
    authored_inventory::ItemObjectives values = held.objectives.value_or(
        authored_inventory::ItemObjectives{});
    const std::int32_t deadline = values[authored_inventory::kItemExpiryLane];
    if (contract.lifetimeSeconds > 0 && (deadline <= 0 || nowSeconds >= deadline)) {
        return 0;
    }
    std::size_t advanced = 0;
    for (const KillRule& rule : rules) {
        std::size_t lane = 0;
        std::int32_t target = 0;
        if (rule.itemHash != held.definitionHash || !matches(rule, kill)
            || !objective_lane(contract, rule.objective, lane, target) || values[lane] < 0
            || values[lane] >= target) {
            continue;
        }
        ++values[lane];
        ++advanced;
    }
    if (advanced != 0) {
        held.objectives = values;
    }
    return advanced;
}

} // namespace

/** Classifies the kill once, then advances the held bounties in one account after-image. */
KillProgress advance_kill_objectives(const KillObservation& kill,
                                     std::int64_t nowSeconds,
                                     const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "not_ready";
    if (!build_data::combat_taxonomy_ready() || !build_data::item_definitions_ready()
        || !build_data::pursuit_definitions_ready()) {
        return KillProgress::notReady;
    }
    KillFacts facts{};
    facts.destination = kill.destination;
    reason = "victim";
    if (!classify_victim(kill, facts)) {
        return KillProgress::refused;
    }
    classify_source(kill, facts);
    investment::store::Transaction transaction;
    const std::unique_ptr<AccountState> saved(new (std::nothrow) AccountState{});
    reason = "account_read";
    if (!transaction.ready() || !saved || !investment::store::read_account(*saved)) {
        return KillProgress::notReady;
    }
    const std::size_t character = selected_character_index(*saved);
    reason = "killer";
    if (character >= saved->characterCount
        || saved->characters[character].soid != kill.killerKey) {
        return KillProgress::refused;
    }
    auto& inventory = saved->characters[character].inventory;
    std::size_t advanced = 0;
    for (auto& held : std::span(inventory.values).first(inventory.count)) {
        advanced += advance_item(held, facts, nowSeconds);
    }
    reason = "no_open_objective";
    if (advanced == 0) {
        return KillProgress::unchanged;
    }
    reason = "account_write";
    if (!account::valid(*saved) || !investment::store::write_account(*saved)
        || !transaction.commit()) {
        return KillProgress::refused;
    }
    reason = nullptr;
    return KillProgress::advanced;
}

} // namespace sunrise::state
