/**
 * Kill objectives: a kill the selected character's player made advances the per-item objective
 * lanes of the held bounties it meets, and the grenade interval Triumph its grenade counts. The
 * victim's race and the killing source's weapon, ability and precision come from the installed
 * combat taxonomy; the destination, slot and damage type come from the incident.
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
#include <string_view>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../unlocks/unlocks_records.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;

namespace {

/** Source kinds of a final blow, as bits; a rule requires every bit it names. */
constexpr std::uint8_t kWeapon = 1;
constexpr std::uint8_t kGrenade = 2;
constexpr std::uint8_t kSuper = 4;
constexpr std::uint8_t kMelee = 8;
/** An ability-group source that carries no weapon label. */
constexpr std::uint8_t kAbility = 16;
constexpr std::uint8_t kPrecision = 32;

// --- Authored game data --------------------------------------------------------------------

// TODO: extract these rows from the packages; no extractor reads which kills an objective counts,
// no extracted item field carries a subclass's element, and no extractor joins a grenade's talent
// to the source label its kills carry.

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

/** The three weapon slots a rule can name, as State's equipment slots. */
enum class WeaponSlot : std::int8_t {
    any = -1,
    kinetic = static_cast<std::int8_t>(authored_inventory::EquipmentSlot::kinetic),
    energy = static_cast<std::int8_t>(authored_inventory::EquipmentSlot::energy),
    heavy = static_cast<std::int8_t>(authored_inventory::EquipmentSlot::heavy),
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

/** The element one subclass item gives the character that equips it. */
struct SubclassElement {
    std::uint32_t itemHash{};
    Element element{};
};

/** Every subclass item and its element. */
constexpr auto kSubclassElements = std::to_array<SubclassElement>({
    {0xB0554739U, Element::arc},
    {0xB920CE9AU, Element::solar},
    {0xC99B33E9U, Element::voidDamage},
    {0x4F91DC97U, Element::arc},
    {0xD8B8D1FCU, Element::solar},
    {0xC0483D8BU, Element::voidDamage},
    {0x686A154AU, Element::arc},
    {0xCF88FEA5U, Element::solar},
    {0xE7BC88B0U, Element::voidDamage},
});

/** An interval Triumph that counts one class's kills with one grenade. */
struct GrenadeTriumph {
    std::uint32_t recordHash{};
    /** Source label the grenade's final blow carries. */
    std::uint32_t sourceLabel{};
    /** Class of the saved character the kill is credited to. */
    CharacterClass characterClass{};
};

/** Grenade source labels, named by their alias rows in the installed source table. */
constexpr std::uint32_t kArcBolt = 0x7EFD858CU;
constexpr std::uint32_t kArcField = 0x342B6EDFU;
constexpr std::uint32_t kArcFlare = 0xE02BBCCBU;
constexpr std::uint32_t kArcFlux = 0x4CA1623CU;
constexpr std::uint32_t kArcProx = 0xADC62A22U;
constexpr std::uint32_t kArcPulse = 0x24263054U;
constexpr std::uint32_t kArcWave = 0x03326E96U;
constexpr std::uint32_t kThermalBolt = 0xC5CC698BU;
constexpr std::uint32_t kThermalField = 0x3D6AF746U;
constexpr std::uint32_t kThermalFlare = 0x7EDE3222U;
constexpr std::uint32_t kThermalFlux = 0x18272D6BU;
constexpr std::uint32_t kThermalProx = 0xD2180725U;
constexpr std::uint32_t kThermalPulse = 0xDC06B92DU;
constexpr std::uint32_t kThermalWave = 0xB8223845U;
constexpr std::uint32_t kVoidBolt = 0xA2E2A1ECU;
constexpr std::uint32_t kVoidField = 0x15D586BFU;
constexpr std::uint32_t kVoidFlare = 0xC1D5D4ABU;
constexpr std::uint32_t kVoidFlux = 0x70867D9CU;
constexpr std::uint32_t kVoidProx = 0xD1AB4582U;
constexpr std::uint32_t kVoidPulse = 0x05D04834U;
constexpr std::uint32_t kVoidWave = 0x27178AF6U;

/** Every grenade interval Triumph, its grenade's source label and its class. */
constexpr auto kGrenadeTriumphs = std::to_array<GrenadeTriumph>({
    {0xB947FBB8U, kVoidFlux, CharacterClass::titan},       // Magnetic
    {0x4C0E42FCU, kVoidWave, CharacterClass::titan},       // Voidwall
    {0x65EDDCE8U, kVoidFlare, CharacterClass::titan},      // Suppressor
    {0x29564F50U, kArcFlare, CharacterClass::titan},       // Flashbang
    {0x9DD4ED6BU, kArcPulse, CharacterClass::titan},       // Pulse
    {0x8697AE16U, kArcProx, CharacterClass::titan},        // Lightning
    {0xAA3ECCB1U, kThermalFlare, CharacterClass::titan},   // Incendiary
    {0xE5429031U, kThermalWave, CharacterClass::titan},    // Thermite
    {0x125469F0U, kThermalFlux, CharacterClass::titan},    // Fusion
    {0x896F7F27U, kVoidPulse, CharacterClass::hunter},     // Vortex
    {0x172957B5U, kVoidProx, CharacterClass::hunter},      // Spike
    {0xCAC54B81U, kVoidWave, CharacterClass::hunter},      // Voidwall
    {0xF4DD42A6U, kArcField, CharacterClass::hunter},      // Skip
    {0x909BE07DU, kArcFlux, CharacterClass::hunter},       // Flux
    {0x0860F593U, kArcBolt, CharacterClass::hunter},       // Arcbolt
    {0x8FBB0303U, kThermalFlare, CharacterClass::hunter},  // Incendiary
    {0x2FABD375U, kThermalField, CharacterClass::hunter},  // Swarm
    {0x2FC2310AU, kThermalProx, CharacterClass::hunter},   // Tripmine
    {0x1467CFBDU, kVoidPulse, CharacterClass::warlock},    // Vortex
    {0xD1DF7507U, kVoidBolt, CharacterClass::warlock},     // Axion Bolt
    {0x657D73A7U, kVoidField, CharacterClass::warlock},    // Scatter
    {0xAF4A1FA0U, kArcBolt, CharacterClass::warlock},      // Arcbolt
    {0x3D248A0EU, kArcPulse, CharacterClass::warlock},     // Pulse
    {0xFD42AE16U, kArcWave, CharacterClass::warlock},      // Storm
    {0xFBCF52E8U, kThermalPulse, CharacterClass::warlock}, // Solar
    {0x51C1A1FAU, kThermalBolt, CharacterClass::warlock},  // Firebolt
    {0xFCD528A0U, kThermalFlux, CharacterClass::warlock},  // Fusion
});

/** A scenario and the bubbles in it a kill may happen in; no scenario places no condition. */
struct Area {
    std::uint32_t scenarioTag{};
    /** Bubble name hashes, one of which must match. The widest objective names three. */
    std::array<std::uint32_t, 3> bubbles{};
};

/** Scenarios the area objectives name, by package tag. */
constexpr std::uint32_t kMarsScenario = 0x80F6AB20U;
constexpr std::uint32_t kMoonScenario = 0x81503E69U;
constexpr std::uint32_t kTangledShoreScenario = 0x80FC9645U;

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
    /** Element of the subclass the killer has equipped, whatever the final blow's; any for none. */
    Element subclassElement{Element::any};
    /** Bubbles the kill must happen in; no scenario for anywhere. */
    Area area{};
};

/** One row per bounty objective lane, in ascending item order; each kill advances a lane by one. */
constexpr auto kKillRules = std::to_array<KillRule>({
    {.itemHash = 0x0337775BU, .destination = kMars, .rankLabel = kBossLabel},
    {.itemHash = 0x0337775CU, .destination = kMars,
     .area = {kMarsScenario, {0x016F2C45U, 0x4AC36E96U, 0x192B0F82U}}},
    {.itemHash = 0x0337775DU, .destination = kMars, .element = Element::voidDamage,
     .sources = kAbility},
    {.itemHash = 0x08CF365AU, .destination = kEdz},
    {.itemHash = 0x0ECA9E33U, .destination = kTitan, .sources = kPrecision},
    {.itemHash = 0x0ECA9E35U, .destination = kTitan, .element = Element::arc, .sources = kAbility},
    {.itemHash = 0x0ECA9E36U, .destination = kTitan, .sources = kWeapon,
     .slot = WeaponSlot::energy},
    {.itemHash = 0x12DB6D98U, .element = Element::solar},
    {.itemHash = 0x12DB6D9BU, .sources = kMelee},
    {.itemHash = 0x12DB6D9EU, .weaponLabel = kBow},
    {.itemHash = 0x14F1A799U, .subclassElement = Element::solar},
    {.itemHash = 0x14F1A79AU, .element = Element::solar, .sources = kWeapon},
    {.itemHash = 0x14F1A79CU, .element = Element::solar, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x1A1BC6CAU, .destination = kNessus, .sources = kGrenade},
    {.itemHash = 0x1D076088U, .destination = kNessus, .sources = kPrecision},
    {.itemHash = 0x20FD444BU, .destination = kNessus, .race = Race::fallen},
    {.itemHash = 0x2233C578U, .anyOf = {.weaponLabels = {kBow, kSubmachineGun, kHandCannon}}},
    {.itemHash = 0x2233C579U, .anyOf = {.weaponLabels = {kSword, kSniperRifle, kBow}}},
    {.itemHash = 0x2233C57AU, .anyOf = {.weaponLabels = {kShotgun}, .slot = WeaponSlot::heavy}},
    {.itemHash = 0x23C1FAD4U, .destination = kIo, .sources = kPrecision},
    {.itemHash = 0x27F60EBFU, .rankLabel = kEliteLabel,
     .anyOf = {.sources = kPrecision | kAbility}},
    {.itemHash = 0x28665292U, .race = Race::cabal},
    {.itemHash = 0x28665299U, .sources = kGrenade},
    {.itemHash = 0x2866529AU, .element = Element::solar, .sources = kWeapon},
    {.itemHash = 0x2866529BU, .race = Race::scorn},
    {.itemHash = 0x28DFBC86U, .destination = kEdz, .sources = kSuper},
    {.itemHash = 0x2D16CE2FU, .destination = kNessus, .element = Element::solar},
    {.itemHash = 0x2E47D6B9U, .destination = kIo, .race = Race::vex},
    {.itemHash = 0x3069FA4BU, .destination = kMars, .sources = kPrecision},
    {.itemHash = 0x321406E2U, .race = Race::taken},
    {.itemHash = 0x321406E3U, .race = Race::fallen},
    {.itemHash = 0x321406E8U, .race = Race::vex},
    {.itemHash = 0x321406EAU, .rankLabel = kBossLabel},
    {.itemHash = 0x321406ECU, .element = Element::voidDamage, .sources = kAbility},
    {.itemHash = 0x321D9F09U, .destination = kEdz, .sources = kGrenade},
    {.itemHash = 0x3314085BU, .race = Race::hive},
    {.itemHash = 0x3314085DU, .weaponLabel = kGrenadeLauncher, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x34D450B5U, .destination = kMars, .element = Element::arc},
    {.itemHash = 0x3B8D6631U, .sources = kSuper},
    {.itemHash = 0x3B8D6638U, .anyOf = {.weaponLabels = {kRocketLauncher}, .sources = kGrenade}},
    {.itemHash = 0x3B8D663AU, .element = Element::voidDamage},
    {.itemHash = 0x3B8D663BU, .element = Element::solar},
    {.itemHash = 0x3B8D663CU, .element = Element::arc},
    {.itemHash = 0x3B8D663EU,
     .anyOf = {.weaponLabels = {kScoutRifle, kSniperRifle, kPulseRifle, kBow}}},
    {.itemHash = 0x3BDD219CU, .destination = kMars, .element = Element::solar},
    {.itemHash = 0x4A71C838U, .destination = kIo, .sources = kMelee},
    {.itemHash = 0x4B3C5716U, .sources = kPrecision},
    {.itemHash = 0x4CAEC586U, .destination = kMercury},
    {.itemHash = 0x4F435712U, .destination = kNessus, .race = Race::cabal},
    {.itemHash = 0x52711B35U, .destination = kMercury, .sources = kGrenade},
    {.itemHash = 0x53FCD57EU, .destination = kNessus, .element = Element::voidDamage},
    {.itemHash = 0x55623448U, .destination = kNessus, .element = Element::arc},
    {.itemHash = 0x5CBBBE3CU, .destination = kIo, .element = Element::arc},
    {.itemHash = 0x5D76391BU, .element = Element::voidDamage, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x5D76391DU, .element = Element::voidDamage, .sources = kWeapon},
    {.itemHash = 0x5D76391EU, .subclassElement = Element::voidDamage},
    {.itemHash = 0x63D3CE9DU, .destination = kMars, .race = Race::hive},
    {.itemHash = 0x6C382BE3U, .destination = kTitan, .element = Element::solar},
    {.itemHash = 0x6DEAB792U, .destination = kIo, .element = Element::voidDamage},
    {.itemHash = 0x6F16E7E3U, .destination = kMercury, .element = Element::arc},
    {.itemHash = 0x7296E6D0U, .sources = kMelee},
    {.itemHash = 0x7296E6D1U, .sources = kPrecision},
    {.itemHash = 0x7296E6D8U, .race = Race::fallen},
    {.itemHash = 0x7296E6D9U, .race = Race::hive},
    {.itemHash = 0x7296E6DAU, .race = Race::cabal},
    {.itemHash = 0x7296E6DBU, .race = Race::scorn},
    {.itemHash = 0x7296E6DCU, .race = Race::taken},
    {.itemHash = 0x7296E6DDU, .race = Race::vex},
    {.itemHash = 0x7296E6DEU, .weaponLabel = kAutoRifle},
    {.itemHash = 0x7296E6DFU, .weaponLabel = kSword},
    {.itemHash = 0x740942DBU, .destination = kEdz, .sources = kPrecision},
    {.itemHash = 0x772B07C1U, .destination = kIo, .sources = kSuper},
    {.itemHash = 0x77F6C3D4U, .destination = kMercury, .race = Race::vex,
     .victimClass = 0xCF060C30U},
    {.itemHash = 0x80BD0E33U, .destination = kIo, .race = Race::taken},
    {.itemHash = 0x80BD0E3CU, .destination = kMercury, .race = Race::vex},
    {.itemHash = 0x80BD0E3DU, .destination = kMars, .race = Race::hive},
    {.itemHash = 0x82640576U, .destination = kIo, .sources = kAbility},
    {.itemHash = 0x82640577U, .destination = kIo, .sources = kWeapon, .slot = WeaponSlot::kinetic},
    {.itemHash = 0x82640577U, .objective = 1, .destination = kIo, .sources = kWeapon,
     .slot = WeaponSlot::energy},
    {.itemHash = 0x82640577U, .objective = 2, .destination = kIo, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x8A48A696U, .destination = kTitan, .sources = kGrenade},
    {.itemHash = 0x8D419E87U, .destination = kMercury, .element = Element::voidDamage},
    {.itemHash = 0x8DF2981FU, .destination = kEdz, .element = Element::arc},
    {.itemHash = 0x8DF57AF1U, .element = Element::arc, .sources = kWeapon,
     .slot = WeaponSlot::heavy},
    {.itemHash = 0x8DF57AF4U, .subclassElement = Element::arc},
    {.itemHash = 0x8DF57AF7U, .element = Element::arc, .sources = kWeapon},
    {.itemHash = 0x8E5D7083U, .destination = kIo, .element = Element::solar},
    {.itemHash = 0x9055E0F2U, .destination = kTitan, .element = Element::voidDamage},
    {.itemHash = 0x93A24325U, .destination = kMars, .sources = kMelee},
    {.itemHash = 0x9507AC48U, .destination = kEdz, .weaponLabel = kSniperRifle},
    {.itemHash = 0x9A412E35U, .destination = kNessus, .weaponLabel = kSniperRifle},
    {.itemHash = 0x9B3F41E5U, .destination = kDreamingCity, .race = Race::taken,
     .rankLabel = kEliteLabel},
    {.itemHash = 0xA6D6C818U, .destination = kTitan, .sources = kMelee | kAbility},
    {.itemHash = 0xAAA306EBU, .destination = kTitan, .race = Race::hive},
    {.itemHash = 0xB5603818U, .race = Race::cabal},
    {.itemHash = 0xB5603818U, .objective = 1, .race = Race::vex},
    {.itemHash = 0xB5603818U, .objective = 2, .race = Race::hive},
    {.itemHash = 0xB5603818U, .objective = 3, .race = Race::taken},
    {.itemHash = 0xB5603818U, .objective = 4, .race = Race::scorn},
    {.itemHash = 0xB5603818U, .objective = 5, .race = Race::fallen},
    {.itemHash = 0xB9550F24U, .destination = kMoon, .weaponLabel = kHandCannon},
    {.itemHash = 0xB9550F29U, .destination = kMoon, .sources = kMelee},
    {.itemHash = 0xB9550F2AU, .destination = kMoon, .weaponLabel = kSubmachineGun},
    {.itemHash = 0xB9550F2BU, .destination = kMoon, .weaponLabel = kAutoRifle},
    {.itemHash = 0xB9550F2CU, .destination = kMoon, .element = Element::solar},
    {.itemHash = 0xB9550F2DU, .destination = kMoon, .element = Element::arc},
    {.itemHash = 0xB9550F2EU, .destination = kMoon, .sources = kGrenade},
    {.itemHash = 0xB9550F2FU, .destination = kMoon, .element = Element::voidDamage},
    {.itemHash = 0xBCE7E301U, .destination = kTitan},
    {.itemHash = 0xBF8448F4U, .destination = kMars, .race = Race::cabal},
    {.itemHash = 0xC10FA21AU, .destination = kMercury, .sources = kSuper},
    {.itemHash = 0xC170B55BU, .destination = kMars, .sources = kGrenade},
    {.itemHash = 0xC506DF36U, .destination = kIo, .sources = kGrenade},
    {.itemHash = 0xC8E05C58U, .destination = kTangledShore},
    {.itemHash = 0xC8E05C59U, .destination = kTangledShore, .race = Race::fallen,
     .area = {kTangledShoreScenario, {0x5FE28198U}}},
    {.itemHash = 0xC8E05C5AU, .destination = kTangledShore, .race = Race::cabal,
     .area = {kTangledShoreScenario, {0xD1CF7944U}}},
    {.itemHash = 0xC8E05C5BU, .destination = kTangledShore, .sources = kPrecision},
    {.itemHash = 0xC8E05C5CU, .destination = kTangledShore, .sources = kSuper},
    {.itemHash = 0xC8E05C5DU, .destination = kTangledShore, .race = Race::hive,
     .area = {kTangledShoreScenario, {0x618294B3U}}},
    {.itemHash = 0xD019F9CDU, .race = Race::vex, .sources = kPrecision},
    {.itemHash = 0xD344C544U, .destination = kMars},
    {.itemHash = 0xD674EC80U, .destination = kMoon, .race = Race::hive,
     .area = {kMoonScenario, {0xFA122719U}}},
    {.itemHash = 0xD674EC82U, .destination = kMoon, .race = Race::hive,
     .area = {kMoonScenario, {0x18E267F9U}}},
    {.itemHash = 0xD674EC83U, .destination = kMoon, .race = Race::fallen,
     .area = {kMoonScenario, {0xC64457D2U}}},
    {.itemHash = 0xD674EC87U, .destination = kMoon, .race = Race::fallen,
     .area = {kMoonScenario, {0xEFEF8119U}}},
    {.itemHash = 0xD72C739CU, .destination = kTitan, .element = Element::arc},
    {.itemHash = 0xDD1D7234U, .destination = kTitan, .sources = kPrecision},
    {.itemHash = 0xE28EF433U, .race = Race::hive},
    {.itemHash = 0xE28EF43CU, .race = Race::cabal},
    {.itemHash = 0xE28EF43DU, .race = Race::fallen},
    {.itemHash = 0xE28EF43EU, .race = Race::taken},
    {.itemHash = 0xE39D657BU, .sources = kWeapon, .slot = WeaponSlot::kinetic},
    {.itemHash = 0xE39D657BU, .objective = 1, .sources = kWeapon, .slot = WeaponSlot::energy},
    {.itemHash = 0xE39D657BU, .objective = 2, .sources = kWeapon, .slot = WeaponSlot::heavy},
    {.itemHash = 0xE68B6181U, .destination = kMercury, .element = Element::solar,
     .sources = kAbility},
    {.itemHash = 0xE71C88D8U, .destination = kMercury, .element = Element::solar,
     .sources = kAbility},
    {.itemHash = 0xE71C88DBU, .destination = kMercury, .race = Race::vex},
    {.itemHash = 0xE71C88DCU, .destination = kMercury, .rankLabel = kBossLabel},
    {.itemHash = 0xE71C88DFU, .destination = kMercury, .sources = kPrecision},
    {.itemHash = 0xE9BE1ECDU, .destination = kNessus, .sources = kSuper},
    {.itemHash = 0xEBBB4B67U, .destination = kEdz, .race = Race::fallen},
    {.itemHash = 0xEBEDB6B5U, .destination = kNessus},
    {.itemHash = 0xEC226745U, .destination = kMars, .element = Element::voidDamage},
    {.itemHash = 0xF5BB6A03U, .destination = kEdz, .element = Element::voidDamage},
    {.itemHash = 0xF78AFDC6U, .destination = kEdz, .race = Race::cabal},
    {.itemHash = 0xFAFC79E1U, .destination = kIo, .race = Race::taken},
    {.itemHash = 0xFC4D8521U, .destination = kTitan, .sources = kSuper},
});

/** An item's rows are found by binary search, which needs the ascending item order. */
static_assert(std::ranges::is_sorted(kKillRules, {}, &KillRule::itemHash));

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
    Element subclassElement{Element::any};
};

/** What one pass over the killer's held items found. */
struct HeldPass {
    /** Lanes the kill advanced. */
    std::size_t advanced{};
    /** Items those lanes belong to. */
    KillAdvancedItems advancedItems{};
    /** Rule rows whose lane was open, before the kill, on a bounty that has not expired. */
    std::bitset<kKillRules.size()> openRules{};
    /** Held items a kill rule names. */
    std::size_t ruledItems{};
    /** The kill's bubble, asked of its reporter the first time a rule names an area. */
    std::optional<KillArea> area{};
    bool areaAsked{};
    /** Static reason an area rule the kill otherwise met did not count; null when none did. */
    const char* areaRefusal{};
};

/** @return The labels in the named group, and the last of them in `member`. */
[[nodiscard]] std::size_t members(std::span<const std::uint32_t> labels,
                                  std::uint32_t group,
                                  std::uint32_t& member) noexcept {
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
 * @return Null for a combatant of a known enemy race, else the static name of the failed check.
 */
[[nodiscard]] const char* victim_refusal(const KillObservation& kill, KillFacts& facts) noexcept {
    if (kill.victimKey != 0) {
        return "victim_player";
    }
    if (!kill.victimCombatant) {
        return "victim_not_combatant";
    }
    if (kill.victimClassHash == 0) {
        return "victim_no_class";
    }
    if (members(kill.victimLabels, kPlayerGroup) != 0) {
        return "victim_player_label";
    }
    facts.victimClass = kill.victimClassHash;
    std::uint8_t ordinal = 0;
    const bool installed = build_data::find_enemy_race(kill.victimClassHash, ordinal);
    if (carries(kill.victimLabels, kScornLabel)) {
        // A class the race table places elsewhere contradicts the label, so neither is trusted.
        if (installed) {
            return "victim_scorn_class";
        }
        facts.race = Race::scorn;
    } else if (!installed) {
        return "victim_class";
    } else if (!enemy_race(static_cast<Race>(ordinal))) {
        return "victim_race";
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
    return nullptr;
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
        std::size_t equipped = 0;
        if (kill.abilitySelector == -1 && kill.equipmentSlot.has_value()
            && semantic_equipment_slot(static_cast<std::uint8_t>(*kill.equipmentSlot), equipped)) {
            const auto slot = static_cast<WeaponSlot>(equipped);
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

/**
 * Asks the reporter for the kill's bubble the first time a rule names an area.
 * @return True when no area is named, or the kill happened in one of its bubbles.
 */
[[nodiscard]] bool in_area(const Area& area, const KillObservation& kill, HeldPass& pass) noexcept {
    if (area.scenarioTag == 0) {
        return true;
    }
    if (!pass.areaAsked) {
        pass.areaAsked = true;
        if (kill.resolveArea != nullptr) {
            pass.area = kill.resolveArea(kill.areaContext);
        }
    }
    if (!pass.area.has_value()) {
        pass.areaRefusal = "area_unbound";
        return false;
    }
    if (pass.area->scenarioTag != area.scenarioTag) {
        // The destination matched, so the authored tag does not name the bound scenario.
        pass.areaRefusal = "scenario_tag";
        return false;
    }
    return std::find(area.bubbles.begin(), area.bubbles.end(), pass.area->bubbleNameHash)
           != area.bubbles.end();
}

/** @return True when the kill meets every condition the rule names, its area aside. */
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
           && (rule.subclassElement == Element::any
               || rule.subclassElement == kill.subclassElement)
           && alternative_matches(rule.anyOf, kill);
}

/** @return True when the kill meets one of the given rule rows, its area aside. */
[[nodiscard]] bool meets_any(const std::bitset<kKillRules.size()>& rules,
                             const KillFacts& kill) noexcept {
    for (std::size_t row = 0; row < kKillRules.size(); ++row) {
        if (rules[row] && matches(kKillRules[row], kill)) {
            return true;
        }
    }
    return false;
}

/** @return The element of the subclass the character has equipped; any when none is known. */
[[nodiscard]] Element equipped_subclass_element(const CharacterState& character) noexcept {
    constexpr auto kSubclassSlot =
        static_cast<std::size_t>(authored_inventory::EquipmentSlot::subclass);
    const auto& subclass = character.equipment.slots[kSubclassSlot];
    if (!subclass.has_value()) {
        return Element::any;
    }
    const auto found = std::find_if(
        kSubclassElements.begin(), kSubclassElements.end(), [&](const SubclassElement& row) {
            return row.itemHash == subclass->definitionHash;
        });
    return found != kSubclassElements.end() ? found->element : Element::any;
}

/**
 * What a read found the selected character had left to advance, kept while the saved revision and
 * the contract revision it was read under both stand. A stale entry may only cause another read:
 * a kill skipped on one is lost, while a needless read is only slow.
 */
struct OpenWork {
    std::uint64_t savedRevision{};
    std::uint64_t contractRevision{};
    /** A read of the held items filled readSeconds, openRules, subclassElement and ruledItems. */
    bool heldRead{};
    /** Server time of that read; an earlier clock could reopen a bounty it found expired. */
    std::int64_t readSeconds{};
    /** Rule rows whose lane that read found open on a held bounty that had not expired. */
    std::bitset<kKillRules.size()> openRules{};
    /** Element of the subclass the character had equipped at that read. */
    Element subclassElement{Element::any};
    /** Held items a kill rule names, as that read counted them. */
    std::size_t ruledItems{};
    /** Grenade Triumph rows a read found already held. */
    std::bitset<kGrenadeTriumphs.size()> heldTriumphs{};
    /**
     * This path wrote under the saved revision, which moves only when the request's transaction
     * commits. Until then a read sees a write a rollback would undo, so nothing more is kept.
     */
    bool written{};
};

/** Read and written only under the store lock. */
OpenWork g_openWork{};

/** @return The kept findings, emptied when the save or the contracts moved after their read. */
[[nodiscard]] OpenWork& current_open_work() noexcept {
    const std::uint64_t saved = investment::store::revision();
    const std::uint64_t contracts = build_data::pursuit_definitions_revision();
    if (g_openWork.savedRevision != saved || g_openWork.contractRevision != contracts) {
        g_openWork = OpenWork{.savedRevision = saved, .contractRevision = contracts};
    }
    return g_openWork;
}

/** @return The class's grenade Triumph row whose label the source carries, else the row count. */
[[nodiscard]] std::size_t carried_triumph(std::span<const std::uint32_t> sourceLabels,
                                          CharacterClass characterClass) noexcept {
    const auto found = std::find_if(
        kGrenadeTriumphs.begin(), kGrenadeTriumphs.end(), [&](const GrenadeTriumph& row) {
            return row.characterClass == characterClass && carries(sourceLabels, row.sourceLabel);
        });
    return static_cast<std::size_t>(found - kGrenadeTriumphs.begin());
}

/**
 * Advances one grenade Triumph and keeps a finding that it is already held.
 * @param row Row of kGrenadeTriumphs whose grenade made the final blow.
 * @param refusal Receives a static reason when the Triumph's record or its steps do not resolve.
 * @return True when the Triumph's progress moved.
 */
[[nodiscard]] bool advance_grenade_triumph(std::size_t row,
                                           OpenWork& work,
                                           const char*& refusal) noexcept {
    const auto advance = unlocks::records::advance_interval(kGrenadeTriumphs[row].recordHash);
    if (advance == unlocks::records::ObjectiveAdvance::alreadyHeld && !work.written) {
        work.heldTriumphs[row] = true;
    }
    if (advance == unlocks::records::ObjectiveAdvance::unavailable) {
        refusal = "triumph_unresolved";
    }
    return unlocks::records::progress_moved(advance);
}

/**
 * Advances every open lane of one held item that the kill meets, and notes its open lanes.
 * @return The lanes advanced.
 */
[[nodiscard]] std::size_t advance_item(authored_inventory::Item& held,
                                       const KillObservation& kill,
                                       const KillFacts& facts,
                                       std::int64_t nowSeconds,
                                       HeldPass& pass) noexcept {
    const auto rules =
        std::ranges::equal_range(kKillRules, held.definitionHash, {}, &KillRule::itemHash);
    if (rules.empty()) {
        return 0;
    }
    ++pass.ruledItems;
    build_data::items::Definition item{};
    build_data::pursuits::Definition contract{};
    if (!build_data::find_item_definition_hash(held.definitionHash, item)
        || !build_data::find_pursuit_definition(item.definitionIndex, contract)
        || !contract.readable) {
        return 0;
    }
    authored_inventory::ItemObjectives values = held.objectives.value_or(
        authored_inventory::ItemObjectives{});
    if (bounty_expired(contract, values, nowSeconds)) {
        return 0;
    }
    std::size_t advanced = 0;
    for (const KillRule& rule : rules) {
        std::size_t lane = 0;
        std::int32_t target = 0;
        if (!objective_lane(contract, rule.objective, lane, target) || values[lane] < 0
            || values[lane] >= target) {
            continue;
        }
        pass.openRules[static_cast<std::size_t>(&rule - kKillRules.data())] = true;
        if (!matches(rule, facts) || !in_area(rule.area, kill, pass)) {
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

/**
 * Advances every held bounty lane the kill meets, gives each changed item a new serial and notes
 * the item.
 * @return False when the character's serials are exhausted.
 */
[[nodiscard]] bool advance_held_bounties(CharacterState& killer,
                                         const KillObservation& kill,
                                         KillFacts& facts,
                                         std::int64_t nowSeconds,
                                         HeldPass& pass) noexcept {
    facts.subclassElement = equipped_subclass_element(killer);
    auto& inventory = killer.inventory;
    for (auto& held : std::span(inventory.values).first(inventory.count)) {
        const std::size_t lanes = advance_item(held, kill, facts, nowSeconds, pass);
        if (lanes == 0) {
            continue;
        }
        if (!next_character_serial(killer, held.mutationSerial)) {
            return false;
        }
        pass.advanced += lanes;
        pass.advancedItems.instanceSoids[pass.advancedItems.count++] = held.instanceSoid;
    }
    return true;
}

} // namespace

/** Checks the killer, then what is left to advance, before judging the kill or reading items. */
KillProgress advance_kill_objectives(const KillObservation& kill,
                                     std::int64_t nowSeconds,
                                     KillReport& report) noexcept {
    report = KillReport{.reason = "not_ready"};
    if (!build_data::combat_taxonomy_ready() || !build_data::item_definitions_ready()
        || !build_data::pursuit_definitions_ready()) {
        return KillProgress::notReady;
    }
    // One lock spans the identity read, the kept findings and the save transaction.
    const std::lock_guard lock(investment::store::g_mutex);
    AccountIdentity identity{};
    report.reason = "identity_read";
    if (!investment::store::read_account_identity(identity)) {
        return KillProgress::notReady;
    }
    const CharacterIdentity* const killer = account::selected_character(identity);
    report.reason = "killer";
    if (killer == nullptr || killer->soid != kill.killerKey) {
        return KillProgress::refused;
    }
    OpenWork& work = current_open_work();
    const bool heldKnown = work.heldRead && nowSeconds >= work.readSeconds;
    const std::size_t triumph = carried_triumph(kill.sourceLabels, killer->characterClass);
    const bool triumphOpen = triumph < kGrenadeTriumphs.size() && !work.heldTriumphs[triumph];
    report.reason = "no_open_objective";
    report.ruledItems = work.ruledItems;
    // With nothing left open, the kill is not judged at all.
    if (heldKnown && work.openRules.none() && !triumphOpen) {
        return KillProgress::unchanged;
    }
    KillFacts facts{};
    facts.destination = kill.destination;
    // The open rules are checked against the subclass their read found; a new read replaces it.
    facts.subclassElement = work.subclassElement;
    report.reason = victim_refusal(kill, facts);
    if (report.reason != nullptr) {
        return KillProgress::refused;
    }
    classify_source(kill, facts);
    const bool readsBounties = !heldKnown || meets_any(work.openRules, facts);
    const bool countsTriumph = triumphOpen && (facts.sources & kGrenade) != 0;
    report.reason = "no_open_objective";
    // A kill that meets no open rule and counts no open Triumph reads nothing more.
    if (!readsBounties && !countsTriumph) {
        return KillProgress::unchanged;
    }
    investment::store::Transaction transaction;
    report.reason = "account_read";
    if (!transaction.ready()) {
        return KillProgress::notReady;
    }
    HeldPass pass{};
    std::unique_ptr<AccountState> saved;
    if (readsBounties) {
        saved.reset(new (std::nothrow) AccountState{});
        if (!saved || !investment::store::read_account(*saved)) {
            return KillProgress::notReady;
        }
        const auto slot = static_cast<std::size_t>(killer - identity.characters.data());
        report.reason = "serial";
        if (!advance_held_bounties(saved->characters[slot], kill, facts, nowSeconds, pass)) {
            return KillProgress::refused;
        }
        report.ruledItems = pass.ruledItems;
    }
    // The Triumph's banks share this transaction, so a refused account write undoes it too.
    const char* triumphRefusal = nullptr;
    const bool triumphAdvanced =
        countsTriumph && advance_grenade_triumph(triumph, work, triumphRefusal);
    if (pass.advanced == 0 && !triumphAdvanced) {
        if (readsBounties && !work.written) {
            work.heldRead = true;
            work.readSeconds = nowSeconds;
            work.openRules = pass.openRules;
            work.subclassElement = facts.subclassElement;
            work.ruledItems = pass.ruledItems;
        }
        report.reason = "no_open_objective";
        if (triumphRefusal != nullptr) {
            report.reason = triumphRefusal;
        }
        if (pass.areaRefusal != nullptr) {
            report.reason = pass.areaRefusal;
        }
        return KillProgress::unchanged;
    }
    report.reason = "account_write";
    if ((pass.advanced != 0
         && (!account::valid(*saved) || !investment::store::write_account(*saved)))
        || !transaction.commit()) {
        return KillProgress::refused;
    }
    work.written = true;
    report.reason = nullptr;
    report.advancedItems = pass.advancedItems;
    report.triumphAdvanced = triumphAdvanced;
    return KillProgress::advanced;
}

} // namespace sunrise::state
