/** Credits a kill report to the held bounties of the character that made it. */

#include "activity_kill_progress.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/runtime/server_clock.h"
#include "../../../../../middleware/bap/activity_message/kill_incident.h"
#include "../../../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/activity_sdk/generated_world/runtime.h"
#include "../../../../../state/activity_sdk/runtime.h"
#include "../../../../../state/build_data/sobjects/sobject_catalog.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../bap/internal.h"

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
           && state::build_data::sobjects::find(static_cast<std::uint16_t>(incident.primaryTarget),
                                                target)
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

} // namespace

/** Decodes the kill, names the live destination, and lets State judge and credit it. */
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
                          "ev=kill_progress result=fail reason=payload bytes=%u",
                          incident.payloadLength);
        return;
    }
    // A kill can arrive on the private activity while its region is the public one, so the
    // destination follows the instantiated region, not the delivering session.
    const std::uint64_t destinationSession =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    state::activity::destination::DestinationSelection destination{};
    static_cast<void>(state::activity::destination::snapshot(destinationSession, destination));
    const std::size_t packageLength = (std::min)(
        static_cast<std::size_t>(destination.packageNameLength), destination.packageName.size());

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
    observation.destination = std::string_view(
        reinterpret_cast<const char*>(destination.packageName.data()), packageLength);
    observation.area = kill_area_locked(destinationSession);

    const char* reason = nullptr;
    const state::KillProgress progress = state::advance_kill_objectives(
        observation, core::runtime::server_clock_seconds(), &reason);
    core::log::writef(core::log::Channel::server,
                      progress == state::KillProgress::advanced ? core::log::Level::info
                                                                : core::log::Level::debug,
                      "ev=kill_progress result=%u reason=%s class=0x%08X labels=%zu/%zu/%zu "
                      "slot=%d damage=%d package=%.*s scenario=0x%08X bubble=0x%08X",
                      static_cast<unsigned>(progress),
                      reason != nullptr ? reason : "advanced",
                      kill.victim.classHash,
                      kill.source.count,
                      kill.actor.count,
                      kill.target.count,
                      kill.equipmentSlot.value_or(-1),
                      kill.damageType,
                      static_cast<int>(observation.destination.size()),
                      observation.destination.data(),
                      observation.area ? observation.area->scenarioTag : 0U,
                      observation.area ? observation.area->bubbleNameHash : 0U);
    if (progress == state::KillProgress::advanced) {
        bap::arm_account_resync_everywhere();
    }
}

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
