#include "state/investment/store_internal.h"
/**
 * Applies the state change a collectible report implies, once its outer body has passed.
 * The incident's own targets name the object. Nothing here reads the running game client.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/crypto/random_bytes.h"
#include "../../../../../middleware/encoding/byte_order.h"
#include "../../../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/build_data/sobjects/sobject_catalog.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../../state/unlocks/unlocks_records.h"
#include "../../../../bap/internal.h"
#include "activity_incident_grants.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

namespace message = middleware::bap::activity_message;

namespace {

namespace unlock_records = state::unlocks::records;

/** One dense run of world-object ordinals mapping onto a dense run of records. */
struct LoreOrdinalRange final {
    std::uint16_t firstOrdinal{};
    std::uint16_t lastOrdinal{};
    std::uint16_t firstRecord{};
};

/** Package names the destination snapshot reports for the two destinations that pay loot. */
constexpr std::string_view kDreamingCityPackage = "dreaming_city_freeroam";
constexpr std::string_view kMoonPackage = "luna_freeroam";

/** One target index shared by every interaction that carries no object of its own. */
constexpr std::uint32_t kGenericInteractionTarget = 3539U;

/** Luna's Lost ghosts occupy one dense ordinal run; the last one sits off the Moon. */
constexpr std::uint16_t kMoonGhostFirstOrdinal = 3310U;
constexpr std::uint16_t kMoonGhostLastOrdinal = 3319U;
constexpr std::uint16_t kMoonDestinationLastOrdinal = 3318U;

/** Objective slot counting the Moon ghosts found. */
constexpr std::uint16_t kLunasLostAreFoundFlag = 10698U;

// --- Authored game data --------------------------------------------------------------------

// TODO: extract these rows from the packages; no extractor relates an incident target to the
// record it reports or to the reward pool its destination pays.

/** Sixteen Owl Sector drones report one dense ordinal run and one sparse record run. */
constexpr std::uint16_t kDroneFirstOrdinal = 2455U;
constexpr std::array<std::uint16_t, 16> kDroneRecords{
    740U,
    741U,
    742U,
    744U,
    746U,
    747U,
    748U,
    749U,
    750U,
    751U,
    752U,
    753U,
    754U,
    755U,
    756U,
    757U,
};

/** Four collectible families whose ordinals and records both run dense. */
constexpr std::array<LoreOrdinalRange, 4> kLoreOrdinalRanges{{
    {2471U, 2493U, 802U},  // Dead Ghosts
    {2494U, 2516U, 778U},  // Awoken crystals
    {2517U, 2532U, 759U},  // Ahamkara bones
    {3310U, 3319U, 1841U}, // Luna's Lost ghosts
}};

/** Dreaming City weapon pool one collectible pays from. */
constexpr std::array<std::uint32_t, 7> kDreamingCityWeapons{
    640114618U,
    334171687U,
    346136302U,
    3242168339U,
    3297863558U,
    3740842661U,
    1644160541U,
};
/** Reverie Dawn armour a Dreaming City collectible pays a Titan. */
constexpr std::array<std::uint32_t, 5> kDreamingCityTitanArmour{
    4097166900U,
    2503434573U,
    4070309619U,
    3174233615U,
    1980768298U,
};
/** Reverie Dawn armour a Dreaming City collectible pays a Hunter. */
constexpr std::array<std::uint32_t, 5> kDreamingCityHunterArmour{
    2824453288U,
    1705856569U,
    1593474975U,
    344548395U,
    3306564654U,
};
/** Reverie Dawn armour a Dreaming City collectible pays a Warlock. */
constexpr std::array<std::uint32_t, 5> kDreamingCityWarlockArmour{
    185695659U,
    2761343386U,
    2859583726U,
    188778964U,
    3602032567U,
};

/** Moon weapon pool one collectible pays from. */
constexpr std::array<std::uint32_t, 9> kMoonWeapons{
    2723909519U,
    2931957300U,
    3924212056U,
    1016668089U,
    1645386487U,
    3325778512U,
    4277547616U,
    3870811754U,
    3690523502U,
};
/** Dreambane armour a Moon collectible pays a Titan. */
constexpr std::array<std::uint32_t, 5> kMoonTitanArmour{
    925079356U,
    2568538788U,
    3312368889U,
    272413517U,
    310888006U,
};
/** Dreambane armour a Moon collectible pays a Hunter. */
constexpr std::array<std::uint32_t, 5> kMoonHunterArmour{
    3571441640U,
    883769696U,
    193805725U,
    659922705U,
    377813570U,
};
/** Dreambane armour a Moon collectible pays a Warlock. */
constexpr std::array<std::uint32_t, 5> kMoonWarlockArmour{
    682780965U,
    3692187003U,
    2048903186U,
    1528483180U,
    1030110631U,
};

// --- Grants --------------------------------------------------------------------------------

/** @return True when the grant moved the account, so the client needs a resync. */
[[nodiscard]] constexpr bool grant_changed(unlock_records::GrantOutcome outcome) noexcept {
    return outcome == unlock_records::GrantOutcome::granted
           || outcome == unlock_records::GrantOutcome::progressed;
}

/** @return True when the objective moved, so the client needs a resync. */
[[nodiscard]] constexpr bool progress_changed(unlock_records::ObjectiveAdvance outcome) noexcept {
    return outcome == unlock_records::ObjectiveAdvance::advanced
           || outcome == unlock_records::ObjectiveAdvance::completed;
}

/** @return True when the target named a record, whether or not the grant moved it. */
[[nodiscard]] constexpr bool record_resolved(unlock_records::GrantOutcome outcome) noexcept {
    return outcome != unlock_records::GrantOutcome::recordNotFound
           && outcome != unlock_records::GrantOutcome::notAChapter;
}

/**
 * Maps one lore world-object ordinal onto the record row it reports.
 * @param ordinal World-object ordinal read from the target's definition.
 * @param record Receives the record row; untouched when no run holds the ordinal.
 * @return False when no authored run holds the ordinal.
 */
[[nodiscard]] bool lore_record_for_ordinal(std::uint16_t ordinal, std::uint16_t& record) noexcept {
    if (ordinal >= kDroneFirstOrdinal) {
        const auto index = static_cast<std::size_t>(ordinal - kDroneFirstOrdinal);
        if (index < kDroneRecords.size()) {
            record = kDroneRecords[index];
            return true;
        }
    }
    for (const LoreOrdinalRange& range : kLoreOrdinalRanges) {
        if (ordinal >= range.firstOrdinal && ordinal <= range.lastOrdinal) {
            record = static_cast<std::uint16_t>(range.firstRecord + ordinal - range.firstOrdinal);
            return true;
        }
    }
    return false;
}

/** Grants the nine Phantasmal Fragments paid by one completed Lost Ghost search. */
void grant_lost_ghost_reward() noexcept {
    // One completed search pays nine of this currency.
    constexpr std::uint32_t kPhantasmalFragmentHash = 443031982U;
    constexpr std::int32_t kRewardQuantity = 9;
    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_hash(kPhantasmalFragmentHash, definition)
        || !bap::arm_world_profile_item_acquisition(definition.definitionIndex, kRewardQuantity)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=world_reward kind=lost_ghost result=fail");
    }
}

/**
 * Grants one installed world weapon or active-class armour piece from the supplied pool.
 * @param weapons Weapon hashes; the pool is refused when empty.
 * @param titanArmour Titan armour hashes. The three class arrays must be the same length.
 * @param hunterArmour Hunter armour hashes.
 * @param warlockArmour Warlock armour hashes.
 */
void grant_random_world_loot(std::span<const std::uint32_t> weapons,
                             std::span<const std::uint32_t> titanArmour,
                             std::span<const std::uint32_t> hunterArmour,
                             std::span<const std::uint32_t> warlockArmour) noexcept {
    if (weapons.empty() || titanArmour.size() != hunterArmour.size()
        || titanArmour.size() != warlockArmour.size()) {
        return;
    }
    const state::AccountState account = state::account_snapshot();
    std::span<const std::uint32_t> armour;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (!account.characters[index].selected) {
            continue;
        }
        switch (account.characters[index].characterClass) {
        case state::CharacterClass::hunter:
            armour = hunterArmour;
            break;
        case state::CharacterClass::warlock:
            armour = warlockArmour;
            break;
        case state::CharacterClass::titan:
        default:
            armour = titanArmour;
            break;
        }
        break;
    }
    const std::size_t hashCount = weapons.size() + armour.size();
    std::array<std::byte, sizeof(std::uint32_t)> randomBytes{};
    if (!middleware::crypto::random::fill(randomBytes)) {
        return;
    }
    const std::uint32_t randomValue = middleware::encoding::read_u32_le(randomBytes);
    const std::size_t first = randomValue % hashCount;
    for (std::size_t offset = 0; offset < hashCount; ++offset) {
        const std::size_t index = (first + offset) % hashCount;
        const std::uint32_t hash =
            index < weapons.size() ? weapons[index] : armour[index - weapons.size()];
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(hash, definition)) {
            continue;
        }
        if (!bap::arm_world_item_acquisition(definition.definitionIndex)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=world_reward kind=item result=fail");
        }
        return;
    }
}

/** Grants one installed Dreaming City weapon or active-class Reverie Dawn armour piece. */
void grant_random_dreaming_city_loot() noexcept {
    grant_random_world_loot(kDreamingCityWeapons,
                            kDreamingCityTitanArmour,
                            kDreamingCityHunterArmour,
                            kDreamingCityWarlockArmour);
}

/** Grants one installed Moon weapon or active-class Dreambane armour piece. */
void grant_random_moon_loot() noexcept {
    grant_random_world_loot(kMoonWeapons, kMoonTitanArmour, kMoonHunterArmour, kMoonWarlockArmour);
}

/** Identifies the shared generic target as a Dreaming City cat-statue interaction. */
[[nodiscard]] bool is_dreaming_city_cat(bool definitionFound,
                                        const state::build_data::sobjects::Definition& definition,
                                        std::string_view packageName) noexcept {
    // Name hash and lane the cat statue's world object carries.
    constexpr std::uint32_t kCatNameHash = 0x7A0FD954U;
    constexpr std::uint32_t kCatLane4 = 0x0011FFFFU;
    if (!definitionFound || definition.typeCode != 2 || definition.nameHash != kCatNameHash
        || definition.lane4 != kCatLane4) {
        return false;
    }
    return packageName == kDreamingCityPackage;
}

/**
 * Identifies a Jade Rabbit interaction by its generic target, statue ordinal, and Moon package.
 * @param incident Accepted incident whose extra targets carry the statue.
 * @param primaryFound True when the primary target resolved to a definition.
 * @param primary Definition of the primary target.
 * @param packageName Package the instantiated region reports.
 * @return True when this incident reports one of the nine statues.
 */
[[nodiscard]] bool is_moon_rabbit(const message::incident::Incident& incident,
                                  bool primaryFound,
                                  const state::build_data::sobjects::Definition& primary,
                                  std::string_view packageName) noexcept {
    // Name hash and lane every generic interaction object carries.
    constexpr std::uint32_t kGenericNameHash = 0x7A0FD954U;
    constexpr std::uint32_t kGenericLane4 = 0x0011FFFFU;
    // The nine statues have different target indices and name hashes, but their type-code-2 world
    // object ordinals form one dense run immediately before Luna's Lost ghosts.
    constexpr std::uint16_t kFirstRabbitOrdinal = 3297U;
    constexpr std::uint16_t kLastRabbitOrdinal = 3305U;
    if (!primaryFound || primary.typeCode != 2 || primary.nameHash != kGenericNameHash
        || primary.lane4 != kGenericLane4) {
        return false;
    }

    bool hasRabbitTarget = false;
    for (std::uint32_t index = 0; index < incident.extraTargetCount; ++index) {
        const std::uint32_t target = incident.extraTargets[index];
        state::build_data::sobjects::Definition rabbit{};
        if (!state::build_data::sobjects::find(static_cast<std::uint16_t>(target), rabbit)
            || rabbit.typeCode != 2 || rabbit.recordRow() != 0xFFFFU) {
            continue;
        }
        const std::uint16_t ordinal = rabbit.loreObjectOrdinal();
        hasRabbitTarget = ordinal >= kFirstRabbitOrdinal && ordinal <= kLastRabbitOrdinal;
        if (hasRabbitTarget) {
            break;
        }
    }
    if (!hasRabbitTarget) {
        return false;
    }

    return packageName == kMoonPackage;
}

/**
 * Resolves an authored lore target and applies Moon ghost side effects once.
 * @param target Target index carried by the incident.
 * @return The chapter grant outcome, or recordNotFound when the target reports no record.
 */
[[nodiscard]] unlock_records::GrantOutcome resolve_lore_target(std::uint32_t target) noexcept {
    state::build_data::sobjects::Definition definition{};
    if (!state::build_data::sobjects::find(static_cast<std::uint16_t>(target), definition)) {
        return unlock_records::GrantOutcome::recordNotFound;
    }

    std::uint16_t record = 0;
    std::uint16_t ordinal = 0;
    if (definition.typeCode == 10) {
        record = definition.recordRow();
    } else if (definition.typeCode == 2) {
        ordinal = definition.loreObjectOrdinal();
        if (!lore_record_for_ordinal(ordinal, record)) {
            return unlock_records::GrantOutcome::recordNotFound;
        }
    } else {
        return unlock_records::GrantOutcome::recordNotFound;
    }

    const auto outcome = unlock_records::grant_chapter(record);
    if (outcome == unlock_records::GrantOutcome::granted && ordinal >= kMoonGhostFirstOrdinal
        && ordinal <= kMoonGhostLastOrdinal) {
        if (ordinal <= kMoonDestinationLastOrdinal) {
            (void)unlock_records::advance_objective(kLunasLostAreFoundFlag);
        }
        grant_lost_ghost_reward();
        // One found ghost pays this much seasonal experience.
        constexpr std::int32_t kBaseExperienceReward = 2500;
        (void)bap::arm_seasonal_experience_presentation(kBaseExperienceReward);
    }
    return outcome;
}

} // namespace

/**
 * Applies the state change one accepted collectible report implies.
 * @param incident Outer-valid incident whose targets name the reported object.
 */
static void stage_incident_grants(const message::incident::Incident& incident) noexcept {
    // Preserve the common-header size gate before acting on the incident.
    if (incident.payloadLength < 13) {
        return;
    }

    // Target index, name hash and lane every Corrupted Egg carries.
    constexpr std::uint32_t kCorruptedEggTarget = 693U;
    constexpr std::uint32_t kCorruptedEggNameHash = 0x179A5E15U;
    constexpr std::uint32_t kCorruptedEggLane4 = 0x0A06FFFFU;
    state::build_data::sobjects::Definition primary{};
    const bool primaryFound = state::build_data::sobjects::find(
        static_cast<std::uint16_t>(incident.primaryTarget), primary);
    const bool isCorruptedEgg =
        incident.primaryTarget == kCorruptedEggTarget && primaryFound && primary.typeCode == 3
        && primary.nameHash == kCorruptedEggNameHash && primary.lane4 == kCorruptedEggLane4;

    std::uint32_t loreTarget = incident.primaryTarget;
    unlock_records::GrantOutcome lore = resolve_lore_target(loreTarget);
    if (!record_resolved(lore)) {
        for (std::uint32_t index = 0; index < incident.extraTargetCount; ++index) {
            loreTarget = incident.extraTargets[index];
            lore = resolve_lore_target(loreTarget);
            if (record_resolved(lore)) {
                break;
            }
        }
    }

    core::log::writef(core::log::Channel::server,
                      core::log::Level::debug,
                      "ev=activity stage=incident_lore target=%u selected=%u extra=%u first=%u "
                      "second=%u third=%u fourth=%u outcome=%u",
                      incident.primaryTarget,
                      loreTarget,
                      incident.extraTargetCount,
                      incident.extraTargetCount > 0 ? incident.extraTargets[0] : 0U,
                      incident.extraTargetCount > 1 ? incident.extraTargets[1] : 0U,
                      incident.extraTargetCount > 2 ? incident.extraTargets[2] : 0U,
                      incident.extraTargetCount > 3 ? incident.extraTargets[3] : 0U,
                      static_cast<unsigned>(lore));

    if (grant_changed(lore)) {
        bap::arm_account_resync_everywhere();
    }
    if (record_resolved(lore)) {
        if (isCorruptedEgg) {
            grant_random_dreaming_city_loot();
        }
        return;
    }
    if (!isCorruptedEgg && incident.primaryTarget != kGenericInteractionTarget) {
        return;
    }

    // An incident can arrive on the private activity while its object belongs to the public
    // region, so the grant follows the instantiated region, not the delivering session.
    const std::uint64_t destinationSession =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    state::activity::destination::DestinationSelection destination{};
    static_cast<void>(state::activity::destination::snapshot(destinationSession, destination));
    const std::size_t packageLength =
        (std::min)(static_cast<std::size_t>(destination.packageNameLength),
                   destination.packageName.size());
    const std::string_view packageName(
        reinterpret_cast<const char*>(destination.packageName.data()), packageLength);

    core::log::writef(core::log::Channel::server,
                      core::log::Level::debug,
                      "ev=activity stage=incident_grant target=%u extra=%u lore=%u type=%d "
                      "lane=0x%08X egg=%u destination=0x%016llX package=%.*s",
                      incident.primaryTarget,
                      incident.extraTargetCount,
                      static_cast<unsigned>(lore),
                      primaryFound ? primary.typeCode
                                   : state::build_data::sobjects::kAbsentTypeCode,
                      primaryFound ? primary.lane4 : 0U,
                      static_cast<unsigned>(isCorruptedEgg),
                      static_cast<unsigned long long>(destinationSession),
                      static_cast<int>(packageName.size()),
                      packageName.data());

    // TODO: grant the Derelict, Menagerie, Tribute Hall and Corrupted Egg chapters once the
    // incident names the object; the shared generic target names the interaction only.
    if (isCorruptedEgg) {
        grant_random_dreaming_city_loot();
    } else if (is_dreaming_city_cat(primaryFound, primary, packageName)) {
        grant_random_dreaming_city_loot();
        // Objective slot counting the cat statues fed.
        constexpr std::uint16_t kRememberYourMannersFlag = 9448U;
        if (progress_changed(unlock_records::advance_objective(kRememberYourMannersFlag))) {
            bap::arm_account_resync_everywhere();
        }
    } else if (is_moon_rabbit(incident, primaryFound, primary, packageName)) {
        grant_random_moon_loot();
        // Objective slot counting the Jade Rabbit statues fed.
        constexpr std::uint16_t kLetThemEatRiceCakesFlag = 10696U;
        if (progress_changed(unlock_records::advance_objective(kLetThemEatRiceCakesFlag))) {
            bap::arm_account_resync_everywhere();
        }
    }
}

/** Commits collectible progress and every reward earned by the same report together. */
void apply_incident_grants(const message::incident::Incident& incident) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return;
    }
    stage_incident_grants(incident);
    if (!transaction.commit()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::error,
                         "ev=incident_grant result=fail reason=store");
    }
}

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
