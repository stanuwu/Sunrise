/** Credits a kill report to the held bounties of the character that made it. */

#include "activity_kill_progress.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/runtime/server_clock.h"
#include "../../../../../middleware/bap/activity_message/kill_incident.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/activity_sdk/generated_world/runtime.h"
#include "../../../../../state/activity_sdk/runtime.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../bap/internal.h"
#include "activity_incident_grants.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

namespace message = middleware::bap::activity_message;

namespace {

/** FNV-1 of "any_kill", the global SObject every kill report targets. */
constexpr std::uint32_t kAnyKillNameHash = 0x324E9A20U;
/** Type code of a kill SObject, whose payload is a kill incident. */
constexpr std::int32_t kKillTypeCode = 1;

/** @return True when the incident targets the SObject every kill reports to. */
[[nodiscard]] bool reports_kill(const message::incident::Incident& incident) noexcept {
    state::build_data::sobjects::Definition target{};
    return incident.primaryTarget <= (std::numeric_limits<std::uint16_t>::max)()
           && state::build_data::find_sobject_definition(
               static_cast<std::uint16_t>(incident.primaryTarget), target)
           && target.nameHash == kAnyKillNameHash && target.typeCode == kKillTypeCode;
}

/**
 * Names the bubble the client has instantiated in one activity session, from the generated world
 * of the activity that session binds. The caller owns the session lock, as every route does.
 * @return Absent when the region, the session's one link or its generated world does not resolve.
 */
[[nodiscard]] std::optional<state::KillArea> kill_area_locked(std::uint64_t sessionId) noexcept {
    namespace sdk = state::activity_sdk;
    const std::int32_t region = state::activity::membership::instantiated_region(
        state::activity::membership::reported_placement(sessionId));
    state::activity::SessionBinding binding{};
    if (region < 0 || !state::activity::snapshot_binding(sessionId, binding)) {
        return std::nullopt;
    }
    std::size_t links = 0;
    const bap::Session* const link = bap::unique_activity_link_locked(binding, links);
    sdk::BoundView bound{};
    sdk::generated_world::GeneratedWorldView world{};
    state::KillArea area{};
    if (link == nullptr
        || sdk::resolve(sdk::snapshot(), {binding, links, link->activity.bindingGeneration}, bound)
               != sdk::Status::ready
        || sdk::generated_world::resolve(bound, world) != sdk::generated_world::BindStatus::ready
        || !sdk::generated_world::region_bubble_name(world, region, area.bubbleNameHash)) {
        return std::nullopt;
    }
    area.scenarioTag = world.scenario_tag();
    return area;
}

/** One kill's bubble lookup, kept so its log line shows what State was told. */
struct AreaLookup {
    std::uint64_t sessionId{};
    std::optional<state::KillArea> area{};
};

/** Resolves the bubble when State asks for it, inside the call that holds the session lock. */
[[nodiscard]] std::optional<state::KillArea> resolve_kill_area(void* context) noexcept {
    auto& lookup = *static_cast<AreaLookup*>(context);
    lookup.area = kill_area_locked(lookup.sessionId);
    return lookup.area;
}

} // namespace

/** Decodes the kill, names the live destination, lets State credit it, and arms its publication. */
void apply_kill_progress(const message::incident::Incident& incident) noexcept {
    if (!reports_kill(incident)) {
        return;
    }
    message::kill_incident::Payload kill{};
    if (incident.payloadLength > incident.payload.size()
        || !message::kill_incident::decode(
            std::span(incident.payload).first(incident.payloadLength), kill)) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=kill_progress stage=decode result=fail bytes=%u",
                          incident.payloadLength);
        return;
    }
    // The destination follows the instantiated region, not the delivering session.
    const LiveDestination destination = live_destination();

    state::KillObservation observation{};
    observation.killerKey = kill.killer.playerKey;
    observation.victimKey = kill.victim.playerKey;
    observation.victimClassHash = kill.victim.classHash;
    observation.victimCombatant = kill.victim.combatant;
    observation.sourceLabels = kill.source.view();
    observation.actorLabels = kill.actor.view();
    observation.victimLabels = kill.target.view();
    observation.equipmentSlot = kill.equipmentSlot;
    observation.abilitySelector = kill.abilitySelector;
    observation.damageType = kill.damageType;
    observation.destination = destination.package_name();
    // The bubble costs a generated-world scan, so State asks only when a held rule names an area.
    AreaLookup lookup{destination.sessionId};
    observation.resolveArea = resolve_kill_area;
    observation.areaContext = &lookup;

    state::KillReport report{};
    const state::KillProgress progress = state::advance_kill_objectives(
        observation, core::runtime::server_clock_seconds(), report);
    core::log::writef(core::log::Channel::server,
                      progress == state::KillProgress::advanced ? core::log::Level::info
                                                                : core::log::Level::debug,
                      "ev=kill_progress result=%s reason=%s ruled=%zu class=0x%08X "
                      "labels=%zu/%zu/%zu slot=%d damage=%d package=%.*s scenario=0x%08X "
                      "bubble=0x%08X",
                      progress == state::KillProgress::advanced    ? "ok"
                      : progress == state::KillProgress::unchanged ? "unchanged"
                      : progress == state::KillProgress::refused   ? "refused"
                                                                   : "not_ready",
                      report.reason != nullptr ? report.reason : "advanced",
                      report.ruledItems,
                      kill.victim.classHash,
                      kill.source.count,
                      kill.actor.count,
                      kill.target.count,
                      kill.equipmentSlot.value_or(-1),
                      kill.damageType,
                      static_cast<int>(observation.destination.size()),
                      observation.destination.data(),
                      lookup.area ? lookup.area->scenarioTag : 0U,
                      lookup.area ? lookup.area->bubbleNameHash : 0U);
    if (progress != state::KillProgress::advanced) {
        return;
    }
    if (report.triumphAdvanced) {
        // Triumph progress lives in the account's unlock banks, which the account image carries
        // along with any bounty the same kill advanced.
        bap::arm_account_resync_everywhere();
        return;
    }
    bap::arm_item_values_refresh_everywhere(
        std::span(report.advancedItems.instanceSoids).first(report.advancedItems.count));
}

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
