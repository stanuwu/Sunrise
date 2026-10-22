#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../state/activity/bubble_authority/runtime.h"
#include "../../../state/activity/entity_slots/member_leases.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/membership/mission_party.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/spawn_sets/spawn_set_catalog.h"
#include "../../bap/runtime.h"
#include "../../gameplay/squad_entity_retirement.h"
#include "../activity_sdk_device_runtime.h"
#include "../activity_sdk_lifetime_runtime.h"
#include "../activity_sdk_mission_runtime.h"
#include "../activity_sdk_scene_spawn.h"
#include "../activity_sdk_squad_runtime.h"
#include "mission_output_route.h"
#include "mission_script_runtime.h"
#include "mission_script_runtime_internal.h"

// The intent fan-out reserves one Host output revision, then asks one typed adapter to encode it.

namespace sunrise::server::activity::mission {
namespace {

namespace devices = activity_sdk_devices;
namespace lifetimes = activity_sdk_lifetime;
namespace scenes = activity_sdk_mission;
namespace squads = activity_sdk_squads;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace squad_auth = middleware::bap::activity_message::squad_auth;

/** Room for the carrier field: its key and one decimal 64-bit generation. */
constexpr std::size_t kCarrierFieldCapacity = 32;

/** One mission admits one party, so its links and their pins fit the party's capacity. */
constexpr std::size_t kRouteCapacity =
    ::sunrise::state::activity::entity_slots::kMemberLeaseRowCount;
/** Joined links that can carry an output. */
using RouteCandidates = std::array<server::bap::ActivityLinkView, kRouteCapacity>;
/** Generations of retained bodies an output follows. */
using RoutePins = std::array<std::uint64_t, kRouteCapacity>;

/** Device statuses of a link that lacks the target, its region or its generation. */
constexpr std::array kDeviceNext{devices::Status::targetUnavailable,
                                 devices::Status::noActivityLink,
                                 devices::Status::staleActivityClient};
constexpr WalkPolicy<devices::Status> kDeviceWalk{devices::Status::queued, kDeviceNext, {}};
/** A scene link may also hold another state; a lease still publishing waits on its link. */
constexpr std::array kSceneNext{scenes::SceneStatus::targetUnavailable,
                                scenes::SceneStatus::noActivityLink,
                                scenes::SceneStatus::staleActivityClient,
                                scenes::SceneStatus::wrongState};
constexpr WalkPolicy<scenes::SceneStatus> kSceneWalk{
    scenes::SceneStatus::queued, kSceneNext, scenes::SceneStatus::missionSeedPending};
/** Squad statuses of a link that lacks the target, its region or its generation. */
constexpr std::array kSquadNext{squads::Status::targetUnavailable,
                                squads::Status::noActivityLink,
                                squads::Status::staleActivityClient};
constexpr WalkPolicy<squads::Status> kSquadWalk{squads::Status::queued, kSquadNext, {}};

/** Records the one deterministic execution attempt for diagnostics. */
void begin_intent_attempt(RuntimeInstance& instance, std::uint64_t now) noexcept {
    if (instance.intentAttempts == 0) {
        instance.firstIntentAttempt = now;
    }
    ++instance.intentAttempts;
}

/** Assigns one exact Host output revision without clearing the durable intent. */
[[nodiscard]] bool assign_delivery_state(RuntimeInstance& instance,
                                         std::uint64_t hostOutputRevision) noexcept {
    mission_state::Snapshot snapshot{};
    const mission_state::Status status =
        mission_state::assign_intent_output(instance.view.binding,
                                            instance.programKey,
                                            instance.missionStateRevision,
                                            instance.durableIntentSequence,
                                            hostOutputRevision,
                                            snapshot);
    if (status != mission_state::Status::ready) {
        lua_vm::fault(instance.vm, "durable mission intent assignment compare was refused");
        instance.programStatus = ProgramStatus::programError;
        log_line(
            core::log::Level::warn, &instance, "intent_assign", mission_state::status_name(status));
        return false;
    }
    accept_mission_state(instance, snapshot);
    return instance.durableHostOutputRevision == hostOutputRevision;
}

/** Reserves one exact Host revision, then makes the durable head own it before enqueue. */
[[nodiscard]] bool reserve_delivery(RuntimeInstance& instance,
                                    host::ScriptableOutputReservation& reservation) noexcept {
    if (!host::reserve_scriptable_output(
            instance.view.binding, reservation, instance.durableIntentSequence)) {
        return false;
    }
    if (!assign_delivery_state(instance, reservation.revision)) {
        if (!host::release_scriptable_output(reservation)) {
            log_line(core::log::Level::warn, &instance, "intent_reserve", "release_refused");
        }
        persist_mission_fault(instance);
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return false;
    }
    instance.expectedScriptableRevision = reservation.revision;
    return true;
}

/**
 * Reserves one Host revision for the intent, or refuses the intent while the program is loaded.
 * @param result Refusal result name.
 * @param reason Refusal reason.
 * @return False when no revision was reserved.
 */
[[nodiscard]] bool
reserve_or_refuse(RuntimeInstance& instance,
                  host::ScriptableOutputReservation& reservation,
                  std::string_view result,
                  std::string_view reason = "host_reservation_unavailable") noexcept {
    if (reserve_delivery(instance, reservation)) {
        return true;
    }
    if (instance.programStatus == ProgramStatus::loaded) {
        refuse_delivery(instance, result, reason, host::EffectOutcome::refused);
    }
    return false;
}

/** Releases State before opening the reserved Host lane after an adapter refusal. */
[[nodiscard]] bool
abandon_reserved_delivery(RuntimeInstance& instance,
                          const host::ScriptableOutputReservation& reservation) noexcept {
    if (!release_delivery_state(instance)) {
        // Keep the Host reservation fail-closed when State cannot remove its ownership.
        persist_mission_fault(instance);
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return false;
    }
    if (!host::release_scriptable_output(reservation)) {
        fault_instance(instance, "Host mission output reservation release was refused");
        instance.programStatus = ProgramStatus::programError;
        log_line(core::log::Level::warn, &instance, "intent_reserve", "release_refused");
        clear_pending_events(instance.view.binding);
        clear_delivery(instance);
        return false;
    }
    return true;
}

/**
 * Enters the first delivery stage once an adapter has queued the request.
 * @param now Service tick the commit deadline is measured from.
 * @param result Diagnostic name of the queued request.
 * @param carrier Generation of the link that carries the request; zero is the program's route.
 */
void await_host_commit(RuntimeInstance& instance,
                       std::uint64_t now,
                       std::string_view result,
                       std::uint64_t carrier) noexcept {
    if (instance.expectedScriptableRevision == 0
        || instance.durableHostOutputRevision != instance.expectedScriptableRevision) {
        fault_delivery(instance,
                       "revision_unavailable",
                       "queued intent did not match durable Host revision ownership");
        return;
    }
    instance.lastIntentStatus = (std::numeric_limits<std::uint16_t>::max)();
    instance.deliveryStage = DeliveryStage::awaitingHostCommit;
    instance.deliveryDeadline = deadline_after(now, kHostCommitTimeoutMs);
    // Only a carrier other than the program's route is named.
    std::array<char, kCarrierFieldCapacity> field{};
    const int written = carrier != 0 && carrier != instance.view.activityClientGeneration
                            ? std::snprintf(field.data(),
                                            field.size(),
                                            "carrier=%llu",
                                            static_cast<unsigned long long>(carrier))
                            : 0;
    log_line(core::log::Level::info,
             &instance,
             "intent",
             result,
             written > 0 ? std::string_view{field.data(), static_cast<std::size_t>(written)}
                         : std::string_view{});
}

/**
 * Awaits the Host commit of a queued device-family request, or releases it and refuses.
 * @param enqueued Diagnostic name when the adapter queued the request.
 * @param refused Refusal result name when the adapter refused it.
 * @param carrier Generation of the link the adapter ran on; zero is the program's route.
 */
void finish_reserved(RuntimeInstance& instance,
                     std::uint64_t now,
                     const host::ScriptableOutputReservation& reservation,
                     devices::Status status,
                     std::string_view enqueued,
                     std::string_view refused,
                     std::uint64_t carrier = 0) noexcept {
    if (status == devices::Status::queued) {
        await_host_commit(instance, now, enqueued, carrier);
    } else if (abandon_reserved_delivery(instance, reservation)) {
        refuse_delivery(
            instance, refused, devices::status_name(status), host::EffectOutcome::refused);
    }
}

/**
 * @return True while a scene-family request is valid but its mission-seed lease has not published.
 * The request stays queued: refusing it drops an answer the client is still owed.
 */
[[nodiscard]] bool scene_lease_still_publishing(scenes::SceneStatus status) noexcept {
    return status == scenes::SceneStatus::missionSeedPending
           || status == scenes::SceneStatus::outputBusy;
}

/**
 * Awaits the Host commit of a queued scene-family request, or releases it and waits or refuses.
 * @param enqueued Diagnostic name when the adapter queued the request.
 * @param refused Refusal result name when the adapter refused it.
 * @param carrier Generation of the link the adapter ran on; zero is the program's route.
 */
void finish_reserved_scene(RuntimeInstance& instance,
                           std::uint64_t now,
                           const host::ScriptableOutputReservation& reservation,
                           scenes::SceneStatus status,
                           std::string_view enqueued,
                           std::string_view refused,
                           std::uint64_t carrier) noexcept {
    if (status == scenes::SceneStatus::queued) {
        await_host_commit(instance, now, enqueued, carrier);
    } else if (!abandon_reserved_delivery(instance, reservation)) {
        return;
    } else if (scene_lease_still_publishing(status)) {
        report_intent_status(instance, kIntentStatusSceneLeasePending, scenes::status_name(status));
    } else {
        refuse_delivery(
            instance, refused, scenes::status_name(status), host::EffectOutcome::refused);
    }
}

/** @return The slot row of one catalog squad, or the absent index. */
[[nodiscard]] std::uint32_t squad_slot_row(const sdk::BoundView& view,
                                           std::uint32_t squadRow) noexcept {
    const auto squads = view.catalog->squads();
    return squadRow < squads.size() ? squads[squadRow].slotIndex : format::kAbsentIndex;
}

/** @return The catalog slot row an intent's output names, or the absent index. */
[[nodiscard]] std::uint32_t intent_slot_row(const sdk::BoundView& view,
                                            const lua_vm::Intent& intent) noexcept {
    switch (intent.kind) {
    case lua_vm::IntentKind::stopAuthoredScene:
    case lua_vm::IntentKind::signalAuthoredScene:
    case lua_vm::IntentKind::activateAuthoredScene:
        return intent.secondRow;
    case lua_vm::IntentKind::actorCommand:
        return squad_slot_row(view, intent.firstRow);
    default:
        return intent.firstRow;
    }
}

/** Reads the wire ClientRef one catalog slot names from the slot and its object. */
[[nodiscard]] std::optional<OutputClientRef> slot_client_ref(const sdk::BoundView& view,
                                                             std::uint32_t slotRow) noexcept {
    return slot_row_client_ref(view.catalog->slots(), view.catalog->objects(), slotRow);
}

/**
 * Moves each pin of a one-bubble target to the link holding that bubble's grant, once
 * `holders_route_pins` says a holder can move one.
 * @param holders Each bubble's holder generation, zero for none.
 * @param pins Pins copied from the binding's retained bodies, rewritten in place.
 */
void follow_holders(const RuntimeInstance& instance,
                    const state::activity::bubble_authority::GrantHolders& holders,
                    std::span<host::ScriptablePin> pins) noexcept;

/**
 * Reads the links where retained bodies for an intent's target already live. An actor command
 * follows only its own squad's body: the policy rows it opens are keyed by that link.
 * @param holders Each bubble's grant holder, which a one-bubble target's pin follows.
 * @return The number of pinned generations written.
 */
[[nodiscard]] std::size_t
intent_pins(const RuntimeInstance& instance,
            const lua_vm::Intent& intent,
            const state::activity::bubble_authority::GrantHolders& holders,
            RoutePins& output) noexcept {
    if (instance.view.catalog == nullptr) {
        return 0;
    }
    const std::optional<OutputClientRef> ref =
        slot_client_ref(instance.view, intent_slot_row(instance.view, intent));
    // A new actor program names the squad its member came from, which may be another object.
    const std::optional<OutputClientRef> parent =
        intent.kind == lua_vm::IntentKind::runActorProgram && intent.active
            ? slot_client_ref(instance.view, squad_slot_row(instance.view, intent.secondRow))
            : std::nullopt;
    std::array<host::ScriptablePin, host::kScriptablePinCapacity> estate{};
    std::size_t estateCount = 0;
    if (!ref.has_value()
        || !host::scriptable_auth_pins(instance.view.binding, estate, estateCount)) {
        return 0;
    }
    follow_holders(instance, holders, std::span(estate).first(estateCount));
    return retained_pins(std::span(estate).first(estateCount),
                         *ref,
                         parent,
                         intent.kind != lua_vm::IntentKind::actorCommand,
                         output);
}

/**
 * The bubbles whose holder can apply an intent's output: a squad's authored bubble, an explicit
 * scene occurrence's bubble, else every bubble the target's object has an occurrence in, in any
 * state. An object with no occurrence names none.
 */
[[nodiscard]] BubbleMask target_bubbles(const sdk::BoundView& view,
                                        const lua_vm::Intent& intent) noexcept {
    if (view.catalog == nullptr) {
        return {};
    }
    switch (intent.kind) {
    case lua_vm::IntentKind::placeSquad: {
        std::uint8_t bubble = 0;
        return squads::authored_bubble(view, intent.firstRow, bubble) == squads::Status::ready
                   ? bubble_bit(bubble)
                   : BubbleMask{};
    }
    case lua_vm::IntentKind::stopAuthoredScene:
    case lua_vm::IntentKind::signalAuthoredScene:
    case lua_vm::IntentKind::activateAuthoredScene: {
        const auto occurrences = view.catalog->occurrences();
        return intent.firstRow < occurrences.size()
                   ? occurrence_bubble(
                         view.catalog->bubbles(), view.scenarioRow, occurrences[intent.firstRow])
                   : BubbleMask{};
    }
    default:
        break;
    }
    const auto slots = view.catalog->slots();
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    const std::uint32_t row = intent_slot_row(view, intent);
    if (scenario == nullptr || row >= slots.size()) {
        return {};
    }
    BubbleMask bubbles{};
    for (const format::Occurrence& occurrence :
         sdk::scenario_occurrences(*view.catalog, *scenario)) {
        if (occurrence.objectIndex == slots[row].objectIndex) {
            bubbles |= occurrence_bubble(view.catalog->bubbles(), view.scenarioRow, occurrence);
        }
    }
    return bubbles;
}

/** Moves each pin of a one-bubble target to the link holding that bubble's grant. */
void follow_holders(const RuntimeInstance& instance,
                    const state::activity::bubble_authority::GrantHolders& holders,
                    std::span<host::ScriptablePin> pins) noexcept {
    const auto live = live_links(instance);
    if (instance.view.catalog == nullptr || !holders_route_pins(live.size())) {
        return;
    }
    for (host::ScriptablePin& pin : pins) {
        pin.expectedActivityClientGeneration =
            grant_holder_pin(pin.expectedActivityClientGeneration,
                             pin_bubbles(instance.pinBubbles, pin),
                             holders,
                             live);
    }
}

/** @return Each bubble's grant holder, all zero while fewer than two links are live. */
[[nodiscard]] state::activity::bubble_authority::GrantHolders
live_grant_holders(const RuntimeInstance& instance) noexcept {
    state::activity::bubble_authority::GrantHolders holders{};
    if (holders_route_pins(live_links(instance).size())) {
        state::activity::bubble_authority::grant_holders(instance.view.binding.sessionId, holders);
    }
    return holders;
}

/** @return The link an actor command's squad body lives on, or the program's route. */
[[nodiscard]] std::uint64_t actor_command_generation(const RuntimeInstance& instance,
                                                     const lua_vm::Intent& intent) noexcept {
    RoutePins pins{};
    const std::size_t pinCount = intent_pins(instance, intent, live_grant_holders(instance), pins);
    RouteCandidates candidates{};
    const std::size_t count =
        server::bap::mission_route_candidates(instance.view.binding,
                                              std::span(pins).first(pinCount),
                                              instance.view.activityClientGeneration,
                                              candidates);
    const auto pinned = std::span(pins).first(pinCount);
    return count != 0
                   && std::find(
                          pinned.begin(), pinned.end(), candidates[0].activityClientGeneration)
                          != pinned.end()
               ? candidates[0].activityClientGeneration
               : instance.view.activityClientGeneration;
}

/**
 * Runs one adapter across the joined links in the rule's order: links already holding a retained
 * body for the target or its squad, because only their reports are admitted; then links whose
 * client is in one of the target's bubbles, because only those apply it; then the route; then the
 * rest. Each adapter refuses a link that lacks the target before it enqueues anything, so every
 * attempt reuses the same unarmed reservation.
 * @param policy The family's statuses that move on to the next link or wait.
 * @param carrier Receives the generation of the link the returned status came from.
 * @param adapter Runs the family's reserved request on one view.
 * @return The status that ended the walk.
 */
template <typename Result, typename Adapter>
[[nodiscard]] Result deliver_through_holder(const RuntimeInstance& instance,
                                            const lua_vm::Intent& intent,
                                            const WalkPolicy<Result>& policy,
                                            std::uint64_t& carrier,
                                            const Adapter& adapter) noexcept {
    const std::uint64_t route = instance.view.activityClientGeneration;
    // One read orders the links and moves the pins, so both name the same holders.
    const state::activity::bubble_authority::GrantHolders holders = live_grant_holders(instance);
    RoutePins pins{};
    // A placement may move its squad's pin, so only its bubble orders it.
    const std::size_t pinCount = intent.kind == lua_vm::IntentKind::placeSquad
                                     ? 0
                                     : intent_pins(instance, intent, holders, pins);
    const auto pinned = std::span(pins).first(pinCount);
    RouteCandidates candidates{};
    const auto ordered = std::span(candidates)
                             .first(server::bap::mission_route_candidates(
                                 instance.view.binding, pinned, route, candidates));
    promote_bubble_holders(ordered, pinned, target_bubbles(instance.view, intent), holders);
    return walk_links(std::span<const server::bap::ActivityLinkView>(ordered),
                      route,
                      policy,
                      carrier,
                      [&](std::uint64_t generation) noexcept {
                          return adapter(routed_view(instance, generation));
                      });
}

} // namespace

/** Moves each pin of a one-bubble target to the link holding that bubble's grant. */
void follow_bubble_grants(const RuntimeInstance& instance,
                          std::span<host::ScriptablePin> pins) noexcept {
    follow_holders(instance, live_grant_holders(instance), pins);
}

/** Reads the pin bubbles of a view's bound catalog and scenario. */
bool bind_pin_bubbles(const sdk::BoundView& view, std::vector<PinBubbleRow>& output) noexcept {
    if (view.catalog == nullptr) {
        std::vector<PinBubbleRow>{}.swap(output);
        return true;
    }
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    return build_pin_bubble_table(
        {.slots = view.catalog->slots(),
         .objects = view.catalog->objects(),
         .squads = view.catalog->squads(),
         .bubbles = view.catalog->bubbles(),
         .occurrences = scenario != nullptr ? sdk::scenario_occurrences(*view.catalog, *scenario)
                                            : std::span<const format::Occurrence>{},
         .scenarioRow = view.scenarioRow},
        [&view](std::uint32_t squadRow, std::uint8_t& bubble) noexcept {
            return squads::authored_bubble(view, squadRow, bubble) == squads::Status::ready;
        },
        output);
}

/** Raises one queued intent, or advances the delivery already in flight. */
void dispatch_intent(RuntimeInstance& instance, std::uint64_t now) noexcept {
    if (instance.programStatus != ProgramStatus::loaded) {
        return;
    }
    lua_vm::Intent intent{};
    if (!lua_vm::pending_intent(instance.vm, intent)) {
        if (instance.deliveryStage != DeliveryStage::idle) {
            fault_delivery(instance, "missing_intent", "delivery lost its pending Lua intent");
        } else {
            clear_delivery(instance);
        }
        return;
    }
    if (intent_lifetime_expired(instance, now)) {
        if (reconcile_expired_delivery(instance)) {
            return;
        }
        refuse_delivery(instance,
                        "intent_timeout",
                        "intent exceeded its delivery lifetime",
                        host::EffectOutcome::expired);
        return;
    }
    if (instance.deliveryStage != DeliveryStage::idle) {
        if (reconcile_transport_stage(instance)) {
            return;
        }
        static_cast<void>(service_delivery_timeout(instance, now));
        return;
    }
    // The native authority may replicate an enemy immediately after receiving its squad Auth.
    // Retain output until every recipient in that state's bubble can decode its entity
    // definitions. A client with no link in the bubble is not waited for: nothing can carry the
    // output there, and a held intent never starts its lifetime.
    if (intent.kind != lua_vm::IntentKind::selectMissionState
        && intent.kind != lua_vm::IntentKind::restartCheckpoint
        && !server::bap::mission_seed_publication_ready(
            instance.view.binding, instance.view.activityClientGeneration, false)) {
        report_intent_status(
            instance, kIntentStatusStateTransitionPending, "peer_state_publication_pending");
        return;
    }
    begin_intent_attempt(instance, now);
    if (intent.attemptGeneration != instance.attempt.generation) {
        refuse_delivery(instance, "attempt_refused", "stale_attempt", host::EffectOutcome::refused);
        return;
    }
    switch (intent.kind) {
    case lua_vm::IntentKind::selectMissionState: {
        if (intent.retirePlacedProps) {
            namespace retirement = server::gameplay::squad_entity_retirement;
            const auto* world = instance.worldView.snapshot();
            const auto placement =
                state::activity::membership::reported_placement(instance.view.binding.sessionId);
            const auto status =
                world == nullptr || instance.publicTarget
                    ? retirement::TransitionStatus::refused
                    : retirement::begin_placed_transition(
                          instance.view,
                          *world,
                          intent.requestKey,
                          state::activity::membership::instantiated_region(placement),
                          intent.effectiveRegion);
            if (status == retirement::TransitionStatus::pending) {
                report_intent_status(
                    instance, kIntentStatusStateTransitionPending, "placed_retirement_pending");
                return;
            }
            if (status != retirement::TransitionStatus::ready) {
                refuse_delivery(instance,
                                "state_refused",
                                "placed_lifetimes_unavailable",
                                host::EffectOutcome::refused);
                return;
            }
        }
        scenes::Snapshot selected{};
        const scenes::Status status =
            scenes::select_state(instance.view,
                                 intent.effectiveRegion,
                                 std::span(intent.seedOmissions).first(intent.seedOmissionCount),
                                 selected);
        if (status == scenes::Status::outputBusy) {
            report_intent_status(
                instance, kIntentStatusSceneOutputBusy, scenes::status_name(status));
            return;
        }
        if (status != scenes::Status::ready || !selected.configured
            || selected.plan.effectiveRegion
                   != static_cast<std::uint32_t>(intent.effectiveRegion)) {
            refuse_delivery(instance,
                            "state_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
            return;
        }
        // The selected effective region is an authored-state key, not a region the client reports.
        // Publishing this lease revision is the completion edge, except when publication waits for
        // arrival: there the teleport is armed first, because arrival closes that window. The
        // move is one host event per selection: a re-dispatch while publication is pending does
        // not move back a player who has since left the region.
        if (instance.armedStateRevision != selected.revision) {
            arm_state_region_teleport(instance, selected.plan);
            instance.armedStateRevision = selected.revision;
        }
        if (!selected.regionArrivalPending
            && (selected.publicationPending || selected.revision == 0
                || selected.publishedRevision != selected.revision)) {
            report_intent_status(
                instance, kIntentStatusStateTransitionPending, "state_transition_pending");
            return;
        }
        // A selected state names its own slice-set region. Until the client transitions there its
        // object registry comes from the loaded slice-set entry, so the new state's objects stay
        // unfindable. Arming the host teleport is the only mid-activity move.
        static_cast<void>(complete_local_effect(instance, "state_selected"));
        return;
    }
    case lua_vm::IntentKind::holdSpawn: {
        // The hold rides the participation record, so the next roster publication carries it.
        ::sunrise::state::activity::membership::set_program_spawn_hold(
            instance.view.binding.sessionId, intent.active);
        static_cast<void>(
            complete_local_effect(instance, intent.active ? "spawn_held" : "spawn_released"));
        return;
    }
    case lua_vm::IntentKind::restartCheckpoint: {
        if (intent.checkpointReleaseRequest == 0
            && instance.attempt.generation == (std::numeric_limits<std::uint64_t>::max)()) {
            refuse_delivery(instance,
                            "checkpoint_refused",
                            "attempt_generation_exhausted",
                            host::EffectOutcome::refused);
            return;
        }
        if (intent.checkpointReleaseRequest != 0) {
            if (::sunrise::state::activity::membership::release_party_hard_wipe(
                    instance.view.binding, intent.checkpointReleaseRequest)) {
                static_cast<void>(complete_local_effect(instance, "checkpoint_reset_ready"));
            } else {
                refuse_delivery(instance,
                                "checkpoint_refused",
                                "wipe_not_active",
                                host::EffectOutcome::refused);
            }
            return;
        }
        namespace data = ::sunrise::state::build_data;
        const auto& destination = instance.view.binding.destination;
        const std::string_view package(
            reinterpret_cast<const char*>(destination.packageName.data()),
            destination.packageNameLength);
        data::scenarios::Definition layout{};
        data::spawn_sets::NameHash spawn{};
        // Revalidate native member lifetimes: a reconnect withdraws its old life evidence.
        refresh_mission_members(instance);
        FireteamLife currentLife{};
        if (!instance.partySnapshotValid) {
            currentLife.add(PlayerLife::unknown);
        }
        for (const auto& member : instance.memberInputs) {
            if (member.present) {
                currentLife.add(member.life.life());
            }
        }
        // A wipe restarts the whole party at an authored spawn set of the region they are in.
        if (instance.publicTarget || !currentLife.all_dead()
            || instance.activeRegion != intent.effectiveRegion
            || !data::find_scenario_layout(package, layout)
            || !data::spawn_sets::find_hash({layout.spawnStem.data(), layout.spawnStemLength},
                                            intent.checkpointSpawnHash,
                                            spawn)
            || spawn.pointCount == 0) {
            refuse_delivery(instance,
                            "checkpoint_refused",
                            "party_or_spawn_unavailable",
                            host::EffectOutcome::refused);
            return;
        }
        ::sunrise::state::activity::membership::MissionParty party{};
        std::size_t partyCount{};
        for (const auto& member : instance.memberInputs) {
            if (member.present) {
                party[partyCount++] = member.life.member;
            }
        }
        if (::sunrise::state::activity::membership::arm_party_hard_wipe(
                instance.view.binding,
                intent.requestKey,
                intent.effectiveRegion,
                intent.checkpointSpawnHash,
                std::span(party).first(partyCount))) {
            static_cast<void>(complete_local_effect(instance, "checkpoint_wipe_armed"));
        } else {
            refuse_delivery(
                instance, "checkpoint_refused", "membership_busy", host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::placeSquad: {
        const std::span<const std::int32_t> counts(intent.squadCounts.data(), intent.squadCount);
        const auto mode = static_cast<squad_auth::Mode>(intent.squadMode);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "squad_refused")) {
            return;
        }
        // Members spawn in the squad's authored bubble, so the client holding it carries them.
        const std::optional<std::uint32_t> spawnRule =
            intent.squadSpawnRuleRow < 0 ? std::nullopt
                                         : std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                                               intent.squadSpawnRuleRow)};
        std::uint64_t carrier = 0;
        const squads::Status status = deliver_through_holder(
            instance, intent, kSquadWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return squads::place_reserved(view,
                                              intent.firstRow,
                                              counts,
                                              mode,
                                              reservation,
                                              std::nullopt,
                                              intent.squadRetireOnReturn,
                                              std::nullopt,
                                              spawnRule);
            });
        if (status == squads::Status::queued) {
            await_host_commit(instance, now, "squad_enqueued", carrier);
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else {
            refuse_delivery(instance,
                            "squad_refused",
                            squads::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::actorCommand: {
        const ActorCommandPolicyStatus status =
            dispatch_actor_command(instance, intent, actor_command_generation(instance, intent));
        if (status == ActorCommandPolicyStatus::queued) {
            static_cast<void>(complete_local_effect(instance, "actor_command_queued"));
        } else {
            refuse_delivery(instance,
                            "actor_command_refused",
                            status == ActorCommandPolicyStatus::unavailable
                                ? "gameplay_policy_unavailable"
                                : "sdk_command_refused",
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::bindCombatantToSquad: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "combatant_binding_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const devices::Status status = deliver_through_holder(
            instance, intent, kDeviceWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return devices::bind_combatant_to_squad_reserved(
                    view, intent.firstRow, reservation);
            });
        finish_reserved(instance,
                        now,
                        reservation,
                        status,
                        "combatant_binding_enqueued",
                        "combatant_binding_refused",
                        carrier);
        return;
    }
    case lua_vm::IntentKind::setObjectActive: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "object_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const devices::Status status = deliver_through_holder(
            instance, intent, kDeviceWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return devices::set_objects_active_reserved(
                    view,
                    intent.firstRow,
                    std::span(intent.burstRows).first(intent.burstRowCount),
                    intent.entryIndex,
                    intent.active,
                    reservation);
            });
        finish_reserved(
            instance, now, reservation, status, "object_enqueued", "object_refused", carrier);
        return;
    }
    case lua_vm::IntentKind::setDeviceChannel: {
        const auto channel = static_cast<scriptable_auth::Type23Channel>(intent.deviceChannel);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "device_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const devices::Status status = deliver_through_holder(
            instance, intent, kDeviceWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return devices::set_channel_reserved(view,
                                                     intent.firstRow,
                                                     channel,
                                                     intent.deviceValue,
                                                     intent.deviceSnap,
                                                     reservation);
            });
        finish_reserved(
            instance, now, reservation, status, "device_enqueued", "device_refused", carrier);
        return;
    }
    case lua_vm::IntentKind::fireTrigger: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "trigger_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const devices::Status status = deliver_through_holder(
            instance, intent, kDeviceWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return devices::fire_trigger_reserved(
                    view, intent.firstRow, reservation, intent.active);
            });
        finish_reserved(
            instance, now, reservation, status, "trigger_enqueued", "trigger_refused", carrier);
        return;
    }
    case lua_vm::IntentKind::playSequence:
    case lua_vm::IntentKind::setCinematicActive: {
        const bool sequence = intent.kind == lua_vm::IntentKind::playSequence;
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(
                instance, reservation, sequence ? "sequence_refused" : "cinematic_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const scenes::SceneStatus status = deliver_through_holder(
            instance, intent, kSceneWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return sequence
                           ? scenes::play_sequence_slot_reserved(view, intent.firstRow, reservation)
                           : scenes::set_cinematic_slot_active_reserved(
                                 view, intent.firstRow, intent.active, reservation);
            });
        if (status == scenes::SceneStatus::queued) {
            await_host_commit(
                instance, now, sequence ? "sequence_enqueued" : "cinematic_enqueued", carrier);
        } else if (!abandon_reserved_delivery(instance, reservation)) {
            return;
        } else if (scene_lease_still_publishing(status)) {
            // The lease numbers say which side moved, read on the link that is still publishing.
            // The status dedup reports them once.
            std::array<char, 176> detail{};
            scenes::Snapshot lease{};
            const scenes::Status leaseStatus = scenes::query(routed_view(instance, carrier), lease);
            const int written =
                std::snprintf(detail.data(),
                              detail.size(),
                              "%s lease=%s configured=%d revision=%llu published=%llu "
                              "pending=%d arrival=%d region=%d plan_region=%u state_row=%u",
                              scenes::status_name(status),
                              scenes::status_name(leaseStatus),
                              lease.configured ? 1 : 0,
                              static_cast<unsigned long long>(lease.revision),
                              static_cast<unsigned long long>(lease.publishedRevision),
                              lease.publicationPending ? 1 : 0,
                              lease.regionArrivalPending ? 1 : 0,
                              lease.effectiveRegion,
                              static_cast<unsigned>(lease.plan.effectiveRegion),
                              static_cast<unsigned>(lease.plan.stateRow));
            report_intent_status(
                instance,
                kIntentStatusSceneLeasePending,
                written > 0 ? std::string_view{detail.data(), static_cast<std::size_t>(written)}
                            : scenes::status_name(status));
        } else {
            refuse_delivery(instance,
                            sequence ? "sequence_refused" : "cinematic_refused",
                            scenes::status_name(status),
                            host::EffectOutcome::refused);
        }
        return;
    }
    case lua_vm::IntentKind::playActorSequence: {
        // The owner is the decision's own slot on every link, so a mismatch refuses before any.
        if (intent.firstRow != intent.sequenceOwner.slotRow) {
            refuse_delivery(instance,
                            "actor_sequence_refused",
                            devices::status_name(devices::Status::invalidSlot),
                            host::EffectOutcome::refused);
            return;
        }
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "actor_sequence_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const devices::Status status = deliver_through_holder(
            instance, intent, kDeviceWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                // The decision pinned the owner on the program's route. Another carrier resolves
                // the same owner on its own link; an owner stale on the route stays stale.
                auto owner = intent.sequenceOwner;
                if (owner.activityClientGeneration == instance.view.activityClientGeneration) {
                    owner.activityClientGeneration = view.activityClientGeneration;
                }
                return devices::play_combatant_sequence_reserved(
                    view, owner, intent.secondRow, reservation);
            });
        finish_reserved(instance,
                        now,
                        reservation,
                        status,
                        "actor_sequence_enqueued",
                        "actor_sequence_refused",
                        carrier);
        return;
    }
    case lua_vm::IntentKind::playPerformance: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "performance_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const scenes::SceneStatus status = deliver_through_holder(
            instance, intent, kSceneWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return scenes::play_performance_slot_reserved(
                    view, intent.firstRow, intent.secondRow, reservation);
            });
        finish_reserved_scene(instance,
                              now,
                              reservation,
                              status,
                              "performance_enqueued",
                              "performance_refused",
                              carrier);
        return;
    }
    case lua_vm::IntentKind::resetObjectives: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "objective_reset_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const scenes::SceneStatus status = deliver_through_holder(
            instance, intent, kSceneWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return scenes::reset_objectives_slot_reserved(view, intent.firstRow, reservation);
            });
        finish_reserved_scene(instance,
                              now,
                              reservation,
                              status,
                              "objective_reset_enqueued",
                              "objective_reset_refused",
                              carrier);
        return;
    }
    case lua_vm::IntentKind::advanceTask:
    case lua_vm::IntentKind::playDialogueCue: {
        const bool dialogue = intent.kind == lua_vm::IntentKind::playDialogueCue;
        const std::uint16_t cueIndex = static_cast<std::uint16_t>(intent.secondRow);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(
                instance, reservation, dialogue ? "dialogue_refused" : "task_refused")) {
            return;
        }
        const std::optional<std::uint32_t> filter =
            intent.dialogueFilterRow < 0 ? std::nullopt
                                         : std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                                               intent.dialogueFilterRow)};
        std::uint64_t carrier = 0;
        const scenes::SceneStatus status = deliver_through_holder(
            instance, intent, kSceneWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return dialogue ? scenes::play_dialogue_cue_slot_reserved(
                                      view, intent.firstRow, cueIndex, reservation, filter)
                                : scenes::activate_task_slot_reserved(
                                      view, intent.firstRow, reservation);
            });
        finish_reserved_scene(instance,
                              now,
                              reservation,
                              status,
                              dialogue ? "dialogue_enqueued" : "task_enqueued",
                              dialogue ? "dialogue_refused" : "task_refused",
                              carrier);
        return;
    }
    case lua_vm::IntentKind::runActorProgram:
    case lua_vm::IntentKind::retireActor:
    case lua_vm::IntentKind::assignCombatObjective: {
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "native_decision_refused", "output_busy")) {
            return;
        }
        std::uint64_t carrier = 0;
        const devices::Status status = deliver_through_holder(
            instance, intent, kDeviceWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return intent.kind != lua_vm::IntentKind::assignCombatObjective
                           ? devices::run_actor_program_reserved(view, intent, reservation)
                           : devices::assign_combat_objective_reserved(view, intent, reservation);
            });
        finish_reserved(instance,
                        now,
                        reservation,
                        status,
                        "native_decision_enqueued",
                        "native_decision_refused",
                        carrier);
        return;
    }
    case lua_vm::IntentKind::setInteractableObject:
    case lua_vm::IntentKind::setGhostLink:
    case lua_vm::IntentKind::watchDamage:
    case lua_vm::IntentKind::applySlotAuth: {
        if (intent.authByteCount == 0 || intent.authByteCount != intent.authBody.size()) {
            refuse_delivery(
                instance, "slot_auth_refused", "invalid_body", host::EffectOutcome::refused);
            return;
        }
        const std::span<const std::byte> body(intent.authBody.data(), intent.authByteCount);
        const std::span<const std::byte> sdkBuild(intent.sdkBuildSha256);
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "slot_auth_refused")) {
            return;
        }
        const host::ScriptableOverrideKind kind =
            intent.kind == lua_vm::IntentKind::setInteractableObject
                ? host::ScriptableOverrideKind::interactableObject
            : intent.kind == lua_vm::IntentKind::setGhostLink
                ? host::ScriptableOverrideKind::ghostLink
            : intent.kind == lua_vm::IntentKind::watchDamage
                ? host::ScriptableOverrideKind::damageWatch
                : host::ScriptableOverrideKind::sdkAuth;
        std::uint64_t carrier = 0;
        const devices::Status status = deliver_through_holder(
            instance, intent, kDeviceWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return devices::apply_auth_reserved(view,
                                                    intent.firstRow,
                                                    intent.objectTag,
                                                    intent.registryKey,
                                                    intent.authSchema,
                                                    intent.slotIndex,
                                                    intent.slotType,
                                                    body,
                                                    intent.authBitCount,
                                                    sdkBuild,
                                                    reservation,
                                                    kind);
            });
        finish_reserved(
            instance, now, reservation, status, "slot_auth_enqueued", "slot_auth_refused", carrier);
        return;
    }
    case lua_vm::IntentKind::setLifetime: {
        if (intent.lifetimeState == mission_state::kCompletedLifetimeState
            && instance.attempt.complete) {
            refuse_delivery(instance,
                            "lifetime_refused",
                            "mission_already_complete",
                            host::EffectOutcome::refused);
            return;
        }
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "lifetime_refused")) {
            return;
        }
        const devices::Status status =
            lifetimes::set_reserved(instance.view, intent.lifetimeState, reservation);
        finish_reserved(
            instance, now, reservation, status, "lifetime_enqueued", "lifetime_refused");
        return;
    }
    case lua_vm::IntentKind::stopAuthoredScene:
    case lua_vm::IntentKind::signalAuthoredScene:
    case lua_vm::IntentKind::activateAuthoredScene: {
        if (intent.kind == lua_vm::IntentKind::activateAuthoredScene && intent.active
            && !scene_spawn_sources_match(instance.view, intent)) {
            refuse_delivery(
                instance, "scene_refused", "source_plan_changed", host::EffectOutcome::refused);
            return;
        }
        host::ScriptableOutputReservation reservation{};
        if (!reserve_or_refuse(instance, reservation, "scene_refused")) {
            return;
        }
        std::uint64_t carrier = 0;
        const scenes::SceneStatus status = deliver_through_holder(
            instance, intent, kSceneWalk, carrier, [&](const sdk::BoundView& view) noexcept {
                return intent.kind == lua_vm::IntentKind::activateAuthoredScene && intent.active
                           ? scenes::activate_authored_scene_spawn_reserved(
                                 view, intent.firstRow, intent.secondRow, reservation)
                           : scenes::activate_authored_scene_reserved(
                                 view,
                                 intent.firstRow,
                                 intent.secondRow,
                                 reservation,
                                 intent.sceneEventKey,
                                 intent.kind == lua_vm::IntentKind::stopAuthoredScene);
            });
        finish_reserved_scene(
            instance, now, reservation, status, "scene_enqueued", "scene_refused", carrier);
        return;
    }
    }
}

} // namespace sunrise::server::activity::mission
