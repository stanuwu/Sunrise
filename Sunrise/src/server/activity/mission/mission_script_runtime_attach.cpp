/**
 * Resolving, opening and starting one mission program for a Host slot.
 * Every function here runs under the mission runtime lock its caller already holds.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/crypto/sha256.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../../state/mods/config/mod_config.h"
#include "../../bap/runtime.h"
#include "../activity_sdk_mission_runtime.h"
#include "../host_runtime.h"
#include "mission_script_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_sdk_bridge.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {
namespace {

/** True when the link, the SDK view and the world view still match what the instance bound. */
[[nodiscard]] bool still_exact(const RuntimeInstance& instance) noexcept {
    server::bap::ActivityLinkView link{};
    lua_vm::WorldGenerationIdentity worldGeneration{};
    return server::bap::activity_link_view(
               instance.view.binding, instance.view.activityClientGeneration, link)
           && link.publicTarget == instance.publicTarget
           && sdk::revalidate(instance.view,
                              instance.view.binding,
                              link.matchingLinks,
                              link.activityClientGeneration)
                  == sdk::Status::ready
           && instance.worldView.activity_sdk_view().catalog == instance.view.catalog
           && instance.worldView.activity_sdk_view().activityRow == instance.view.activityRow
           && instance.worldView.activity_sdk_view().activityClientGeneration
                  == instance.view.activityClientGeneration
           && sdk_bridge::world_generation_identity(instance.worldView, worldGeneration);
}

/**
 * Rebuilds one open program view for its exact owning ActivityClient generation.
 * @return True when the instance holds an exact view of the same program.
 */
[[nodiscard]] bool rebind_instance(RuntimeInstance& instance) noexcept {
    server::bap::ActivityLinkView link{};
    if ((!server::bap::activity_link_view(
             instance.view.binding, instance.view.activityClientGeneration, link)
         || !link.joined)
        && (!server::bap::mission_activity_link_view(instance.view.binding, link)
            || !link.joined)) {
        return false;
    }
    const sdk::Snapshot catalog = sdk::snapshot();
    // A different SDK build is a different module, so that program opens again.
    if (catalog == nullptr || catalog != instance.view.catalog) {
        return false;
    }
    sdk::BoundView view{};
    const sdk::Selection selection{
        .binding = instance.view.binding,
        .matchingLinks = link.matchingLinks,
        .activityClientGeneration = link.activityClientGeneration,
    };
    if (sdk::resolve(catalog, selection, view) != sdk::Status::ready
        || view.activityRow != instance.view.activityRow) {
        return false;
    }
    generated::GeneratedWorldView worldView{};
    if (generated::resolve(view, worldView) != generated::BindStatus::ready) {
        return false;
    }
    // The pin bubbles depend only on the catalog and scenario, and the catalog is the same here.
    if (view.scenarioRow != instance.view.scenarioRow) {
        std::vector<PinBubbleRow> pinBubbles{};
        if (!bind_pin_bubbles(view, pinBubbles)) {
            return false;
        }
        instance.pinBubbles = std::move(pinBubbles);
    }
    const bool sourceChanged =
        view.activityClientGeneration != instance.view.activityClientGeneration;
    instance.view = std::move(view);
    instance.worldView = std::move(worldView);
    instance.publicTarget = link.publicTarget;
    instance.playerKey = link.playerKey;
    instance.identity.playerKey = link.playerKey;
    instance.identity.publicTarget = link.publicTarget;
    // The bridge copies the world generation, so hand the program the rebuilt pair.
    if (!lua_vm::rebind(instance.vm,
                        instance.identity,
                        sdk_bridge::definition_api(instance.view, instance.worldView))) {
        return false;
    }
    if (sourceChanged) {
        // The next native report establishes its own baseline, not a delta from another client.
        reset_shared_observations(instance);
        instance.activeRegion = link.sliceSetIndex;
    }
    mission_state::Snapshot snapshot{};
    if (!mission_state::state_snapshot(instance.view.binding, snapshot)) {
        return false;
    }
    accept_mission_state(instance, snapshot);
    return true;
}

/** Builds the exact durable program identity from the already validated SDK view. */
[[nodiscard]] bool make_program_key(const RuntimeInstance& instance,
                                    const format::Activity& activity,
                                    std::span<const char> source,
                                    mission_state::ProgramKey& output) noexcept {
    output = {};
    if (instance.view.catalog == nullptr
        || activity.activityIndex
               > static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)())) {
        return false;
    }
    const std::span<const std::byte> digest = instance.view.catalog->sdk_build_sha256();
    if (digest.size() != output.sdkBuildSha256.size()) {
        return false;
    }
    std::copy(digest.begin(), digest.end(), output.sdkBuildSha256.begin());
    if (!sdk_bridge::world_program_generation_sha256(instance.worldView,
                                                     output.worldGenerationSha256)) {
        output = {};
        return false;
    }
    output.worldScenarioTag = instance.worldView.scenario_tag();
    if (!middleware::crypto::sha256::hash(std::as_bytes(source), output.scriptSourceSha256)) {
        output = {};
        return false;
    }
    output.activityDefinition = activity.definitionHash;
    output.activityIndex = static_cast<std::int16_t>(activity.activityIndex);
    output.publicTarget = instance.publicTarget;
    return true;
}

/** Binds and restores the durable record before the opened program enters a callback. */
[[nodiscard]] bool bind_mission_state(RuntimeInstance& instance, std::uint64_t now) noexcept {
    mission_state::Snapshot snapshot{};
    const mission_state::Status status =
        mission_state::bind(instance.view.binding, instance.programKey, snapshot);
    note_vm_status(instance, "state", mission_state::status_name(status));
    if (status != mission_state::Status::ready) {
        log_line(
            core::log::Level::warn, &instance, "state_bind", mission_state::status_name(status));
        lua_vm::fault(instance.vm, "authoritative mission State binding was refused");
        return false;
    }
    instance.missionStateBound = true;
    accept_mission_state(instance, snapshot);
    if (snapshot.state.variableCount > snapshot.state.variables.size()
        || snapshot.state.timerCount > snapshot.state.timers.size()) {
        fault_instance(instance, "authoritative mission State durable row count is invalid");
        return false;
    }
    std::vector<lua_vm::Intent> restoredIntents{};
    try {
        restoredIntents.reserve(snapshot.state.pendingIntents.size());
        for (const mission_state::PendingIntent& pending : snapshot.state.pendingIntents) {
            restoredIntents.push_back(pending.value);
        }
    } catch (const std::bad_alloc&) {
        fault_instance(instance, "authoritative mission State restore allocation failed");
        return false;
    }
    if (!lua_vm::restore_state(instance.vm,
                               snapshot.state.phase,
                               snapshot.state.revision,
                               {snapshot.state.variables.data(), snapshot.state.variableCount},
                               {snapshot.state.timers.data(), snapshot.state.timerCount},
                               snapshot.state.nextTimerSequence,
                               snapshot.state.nextIntentKey,
                               restoredIntents)) {
        fault_instance(instance, "authoritative mission State restore was refused");
        log_line(core::log::Level::warn, &instance, "state_restore", "vm_refused");
        return false;
    }
    if (instance.durableHostOutputRevision != mission_state::kAbsentHostOutputRevision) {
        instance.expectedScriptableRevision = instance.durableHostOutputRevision;
        instance.deliveryStage = DeliveryStage::awaitingHostCommit;
        instance.deliveryDeadline = deadline_after(now, kHostCommitTimeoutMs);
        instance.firstIntentAttempt = now;
        instance.intentAttempts = 1;
        host::InstanceSnapshot hostView{};
        if (host::instance_snapshot(instance.view.binding, hostView) && hostView.outputPending
            && hostView.outputKind == host::OutputKind::scriptableOverride
            && hostView.scriptableRevision == instance.expectedScriptableRevision) {
            instance.deliveryStage = DeliveryStage::awaitingTransport;
            instance.deliveryDeadline = deadline_after(now, kTransportTimeoutMs);
        } else if (hostView.scriptableRevision == instance.expectedScriptableRevision
                   && !hostView.outputPending
                   && hostView.scriptableTransportRevision != instance.expectedScriptableRevision) {
            instance.deliveryDeadline = now;
        }
    }
    if (snapshot.state.faulted) {
        lua_vm::fault(instance.vm, "authoritative mission State is faulted");
        log_line(core::log::Level::warn, &instance, "state_restore", "faulted");
        return false;
    }
    log_line(core::log::Level::debug,
             &instance,
             "state_restore",
             snapshot.state.started ? "started" : "ready");
    return true;
}

/**
 * Finishes one on_load call after same-session VM reattachment.
 * A restore is not a state transition, so an unchanged candidate must not spend a revision.
 * @return True when the program is running.
 */
[[nodiscard]] bool apply_load(RuntimeInstance& instance, lua_vm::CallStatus loaded) noexcept {
    lua_vm::Snapshot diagnostics{};
    lua_vm::snapshot(instance.vm, diagnostics);
    if (loaded != lua_vm::CallStatus::committed && loaded != lua_vm::CallStatus::noHandler) {
        instance.programStatus = ProgramStatus::programError;
        log_line(core::log::Level::warn,
                 &instance,
                 "load",
                 lua_vm::status_name(loaded),
                 {},
                 diagnostics.lastError.data());
        persist_mission_fault(instance);
        return false;
    }
    if (diagnostics.stateRevision != instance.missionStateRevision
        && !commit_mission_state(instance, true, instance.lastMissionSequence)) {
        return false;
    }
    instance.lastLoggedRevision = instance.missionStateRevision;
    return true;
}

enum class InitialStateGate : std::uint8_t {
    ready,
    pending,
    failed,
};

/** Selects the program-declared state once, then waits for its exact roster revision to publish. */
[[nodiscard]] InitialStateGate initial_state_gate(RuntimeInstance& instance) noexcept {
    if (!instance.initialStateDeclared) {
        return InitialStateGate::ready;
    }
    activity_sdk_mission::Snapshot seed{};
    std::array<sdk::MissionSeedOmission, sdk::kMissionSeedOmitCapacity> omissions{};
    std::size_t omissionCount = 0;
    if (!instance.initialStateSelected
        && !lua_vm::initial_state_omissions(instance.vm, omissions, omissionCount)) {
        fault_instance(instance, "program initial_state omit list could not be read");
        return InitialStateGate::failed;
    }
    const activity_sdk_mission::Status status =
        instance.initialStateSelected
            ? activity_sdk_mission::query(instance.view, seed)
            : activity_sdk_mission::select_state(instance.view,
                                                 instance.initialStateRegion,
                                                 std::span(omissions).first(omissionCount),
                                                 seed);
    if (status == activity_sdk_mission::Status::outputBusy) {
        return InitialStateGate::pending;
    }
    if (status != activity_sdk_mission::Status::ready || !seed.configured
        || seed.plan.effectiveRegion != static_cast<std::uint32_t>(instance.initialStateRegion)) {
        log_line(core::log::Level::warn,
                 &instance,
                 "initial_state",
                 activity_sdk_mission::status_name(status));
        fault_instance(instance, "program initial_state mission-seed selection was refused");
        return InitialStateGate::failed;
    }
    instance.initialStateSelected = true;
    // The launch lands in the declared initial region, so the selection owes no move, and a
    // player who walks out of that region afterwards is not moved back.
    // `plan.effectiveRegion` is the authored-state key; several authored states share one client
    // slice-set region, so the client link cannot report it. The lease revision reaching the
    // transport is the publication acknowledgement.
    return seed.publicationPending || seed.revision == 0 || seed.publishedRevision != seed.revision
                   || !server::bap::mission_seed_publication_ready(
                       instance.view.binding, instance.view.activityClientGeneration, true)
               ? InitialStateGate::pending
               : InitialStateGate::ready;
}

/** Runs and durably commits a fresh program only after its initial-state gate is open. */
[[nodiscard]] bool start_program(RuntimeInstance& instance, std::uint64_t now) noexcept {
    const lua_vm::CallStatus started = lua_vm::start(instance.vm, now);
    note_vm_status(instance, "start", lua_vm::status_name(started));
    if (started != lua_vm::CallStatus::committed && started != lua_vm::CallStatus::noHandler) {
        instance.programStatus = ProgramStatus::programError;
        lua_vm::Snapshot diagnostics{};
        lua_vm::snapshot(instance.vm, diagnostics);
        log_line(core::log::Level::warn,
                 &instance,
                 "start",
                 lua_vm::status_name(started),
                 {},
                 diagnostics.lastError.data());
        persist_mission_fault(instance);
        return false;
    }
    if (!commit_mission_state(instance, true, instance.lastMissionSequence)) {
        return false;
    }
    instance.startPending = false;
    instance.lastLoggedRevision = instance.missionStateRevision;
    log_line(core::log::Level::info, &instance, "open", "ready");
    return true;
}

/** Opens the program for one slot, binds its durable record, then reattaches or starts it. */
[[nodiscard]] AttachResult open_program(RuntimeInstance& instance, std::uint64_t now) noexcept {
    const format::Activity* const activity = sdk::bound_activity(instance.view);
    if (activity == nullptr
        || !sdk_bridge::program_identity(instance.view, instance.publicTarget, instance.identity)) {
        instance.programStatus = ProgramStatus::programError;
        note_vm_status(instance, "open", "invalid_sdk_view");
        log_line(core::log::Level::warn, &instance, "open", "invalid_sdk_view");
        return AttachResult::programError;
    }
    instance.identity.sdkLuaSearchPath = sdk_lua_search_path();
    instance.identity.playerKey = instance.playerKey;
    instance.identity.readModConfig = &state::mods::config::read;
    instance.identity.readModuleOwner = &module_source_owner;
    std::span<const char> source{};
    switch (read_source(*instance.view.catalog, *activity, source, instance.identity.modId)) {
    case SourceStatus::missing:
        instance.programStatus = ProgramStatus::missing;
        note_vm_status(instance, "open", "no_script");
        log_line(core::log::Level::info, &instance, "open", "no_script");
        return AttachResult::noScript;
    case SourceStatus::fileError:
        instance.programStatus = ProgramStatus::fileError;
        note_vm_status(instance, "open", "file_error");
        log_line(core::log::Level::warn, &instance, "open", "file_error");
        return AttachResult::scriptFileError;
    case SourceStatus::tooLarge:
        instance.programStatus = ProgramStatus::sourceTooLarge;
        note_vm_status(instance, "open", "source_too_large");
        log_line(core::log::Level::warn, &instance, "open", "source_too_large");
        return AttachResult::sourceTooLarge;
    case SourceStatus::ready:
        break;
    }
    if (!make_program_key(instance, *activity, source, instance.programKey)) {
        clear_source(source);
        instance.programStatus = ProgramStatus::programError;
        note_vm_status(instance, "state", "invalid_program_key");
        log_line(core::log::Level::warn, &instance, "open", "invalid_program_key");
        return AttachResult::programError;
    }
    const lua_vm::OpenStatus opened =
        lua_vm::open(instance.vm,
                     instance.identity,
                     sdk_bridge::definition_api(instance.view, instance.worldView),
                     source);
    clear_source(source);
    note_vm_status(instance, "open", lua_vm::status_name(opened));
    if (opened != lua_vm::OpenStatus::ready) {
        instance.programStatus = ProgramStatus::programError;
        lua_vm::Snapshot diagnostics{};
        lua_vm::snapshot(instance.vm, diagnostics);
        log_line(core::log::Level::warn,
                 &instance,
                 "open",
                 lua_vm::status_name(opened),
                 {},
                 diagnostics.lastError.data());
        return AttachResult::programError;
    }
    instance.programStatus = ProgramStatus::loaded;
    // on_load and on_start may read a device request as applied, which needs the live links now.
    static_cast<void>(read_live_links(instance));
    if (!publish_device_sources(instance, instance.vm)) {
        lua_vm::fault(instance.vm, "device request source allocation failed");
        instance.programStatus = ProgramStatus::programError;
        return AttachResult::programError;
    }
    // The first read records the current peers, so a reattach never replays them.
    push_session_roster_edges(instance);
    instance.initialStateDeclared =
        lua_vm::initial_state_region(instance.vm, instance.initialStateRegion);
    if (instance.initialStateDeclared) {
        instance.activeRegion = instance.initialStateRegion;
        // The host names the arrival slice set; a launched activity's client names none.
        state::activity::membership::note_declared_initial_region(instance.view.binding.sessionId,
                                                                  instance.initialStateRegion);
        std::uint32_t spawnSet = 0;
        static_cast<void>(lua_vm::initial_state_spawn_set(instance.vm, spawnSet));
        // Zero leaves the client on the set its own region names.
        state::activity::membership::note_declared_spawn_set(instance.view.binding.sessionId,
                                                             spawnSet);
    }
    if (!bind_mission_state(instance, now)) {
        instance.programStatus = ProgramStatus::programError;
        return AttachResult::programError;
    }
    if (instance.missionStarted) {
        instance.missionReattached = true;
        const lua_vm::CallStatus loaded = lua_vm::load(instance.vm, now);
        note_vm_status(instance, "load", lua_vm::status_name(loaded));
        if (!apply_load(instance, loaded)) {
            return AttachResult::programError;
        }
        log_line(core::log::Level::info, &instance, "open", "ready", "reason=state_reattached");
        return AttachResult::ready;
    }
    switch (initial_state_gate(instance)) {
    case InitialStateGate::failed:
        return AttachResult::programError;
    case InitialStateGate::pending:
        instance.startPending = true;
        log_line(core::log::Level::info, &instance, "initial_state", "publication_pending");
        return AttachResult::ready;
    case InitialStateGate::ready:
        break;
    }
    return start_program(instance, now) ? AttachResult::ready : AttachResult::programError;
}

/** Binds one host instance to a free slot once its link, SDK view and world view all resolve. */
void attach_instance(const host::InstanceSnapshot& hostInstance,
                     std::span<const state::activity::SessionRosterRow> roster,
                     const sdk::Snapshot& catalog,
                     std::uint64_t now) noexcept {
    if (find_instance(hostInstance.binding) != nullptr) {
        return;
    }
    if (catalog == nullptr) {
        report_attach_result(
            hostInstance.binding, AttachResult::catalogUnavailable, "catalog_unavailable");
        return;
    }
    server::bap::ActivityLinkView link{};
    // Host snapshots are session-scoped and have no client owner yet. The lowest joined native
    // member owns the session-wide program: the primary while present, so a guest sharing its
    // binding cannot replace it, and the first remaining peer once the primary has left.
    std::uint64_t memberKey{};
    for (const auto& row : roster) {
        if (row.joined && state::activity::same_binding(row.binding, hostInstance.binding)) {
            memberKey = row.presentMemberKey;
            break;
        }
    }
    if (!server::bap::activity_link_view_for_member(hostInstance.binding, memberKey, link)) {
        report_attach_result(
            hostInstance.binding, AttachResult::noActivityLink, "no_activity_link");
        return;
    }
    if (!link.joined) {
        report_attach_result(
            hostInstance.binding, AttachResult::noActivityLink, "activity_join_pending");
        return;
    }
    sdk::BoundView view{};
    const sdk::Selection selection{
        .binding = hostInstance.binding,
        .matchingLinks = link.matchingLinks,
        .activityClientGeneration = link.activityClientGeneration,
    };
    const sdk::Status status = sdk::resolve(catalog, selection, view);
    if (status != sdk::Status::ready) {
        report_attach_result(hostInstance.binding,
                             AttachResult::sdkStatus,
                             sdk::status_name(status),
                             format::kAbsentIndex,
                             status);
        return;
    }
    generated::GeneratedWorldView worldView{};
    const generated::BindStatus worldStatus = generated::resolve(view, worldView);
    if (worldStatus != generated::BindStatus::ready) {
        report_attach_result(hostInstance.binding,
                             AttachResult::generatedWorldStatus,
                             generated::status_name(worldStatus),
                             view.activityRow,
                             sdk::Status::notReady,
                             worldStatus);
        return;
    }
    RuntimeInstance* const instance = free_instance();
    if (instance == nullptr) {
        report_attach_result(
            hostInstance.binding, AttachResult::capacity, "capacity", view.activityRow);
        return;
    }
    std::vector<PinBubbleRow> pinBubbles{};
    if (!bind_pin_bubbles(view, pinBubbles)) {
        report_attach_result(
            hostInstance.binding, AttachResult::capacity, "pin_bubble_capacity", view.activityRow);
        return;
    }
    instance->view = std::move(view);
    instance->pinBubbles = std::move(pinBubbles);
    instance->worldView = std::move(worldView);
    instance->publicTarget = link.publicTarget;
    instance->playerKey = link.playerKey;
    instance->occupied = true;
    const AttachResult opened = open_program(*instance, now);
    report_attach_result(
        hostInstance.binding, opened, attach_result_name(opened), instance->view.activityRow);
}

} // namespace

/** Opens new code against the instance's view; the running program is untouched. */
lua_vm::OpenStatus prepare_reload_program(const RuntimeInstance& instance,
                                          std::span<const char> source,
                                          std::string_view modId,
                                          lua_vm::Vm& vm,
                                          lua_vm::ProgramIdentity& identity,
                                          mission_state::ProgramKey& key) noexcept {
    const format::Activity* const activity = sdk::bound_activity(instance.view);
    if (activity == nullptr
        || !sdk_bridge::program_identity(instance.view, instance.publicTarget, identity)
        || !make_program_key(instance, *activity, source, key)) {
        return lua_vm::OpenStatus::invalidProgram;
    }
    identity.sdkLuaSearchPath = sdk_lua_search_path();
    identity.playerKey = instance.playerKey;
    identity.readModConfig = &state::mods::config::read;
    identity.readModuleOwner = &module_source_owner;
    copy_text(identity.modId, modId);
    return lua_vm::open(
        vm, identity, sdk_bridge::definition_api(instance.view, instance.worldView), source);
}

/** Points a prepared VM at the current view; false when the program key changed. */
bool rebind_reload_program(const RuntimeInstance& instance,
                           std::span<const char> source,
                           lua_vm::Vm& vm,
                           lua_vm::ProgramIdentity& identity,
                           const mission_state::ProgramKey& key) noexcept {
    const format::Activity* const activity = sdk::bound_activity(instance.view);
    mission_state::ProgramKey current{};
    lua_vm::ProgramIdentity rebound{};
    if (activity == nullptr || !still_exact(instance)
        || !make_program_key(instance, *activity, source, current) || current != key
        || !sdk_bridge::program_identity(instance.view, instance.publicTarget, rebound)) {
        return false;
    }
    rebound.sdkLuaSearchPath = identity.sdkLuaSearchPath;
    rebound.playerKey = instance.playerKey;
    rebound.readModConfig = identity.readModConfig;
    rebound.readModuleOwner = identity.readModuleOwner;
    rebound.modId = identity.modId;
    if (!lua_vm::rebind(
            vm, rebound, sdk_bridge::definition_api(instance.view, instance.worldView))) {
        return false;
    }
    identity = rebound;
    return true;
}

/** Drops slots that no longer match, publishes the roster, and attaches active host instances. */
void synchronize_instances(std::uint64_t now) noexcept {
    host::DiagnosticsSnapshot diagnostics{};
    host::snapshot(diagnostics);
    retire_attach_diagnostics(diagnostics);
    retire_unbound_pending_events();
    for (RuntimeInstance& instance : g_instances) {
        const bool bindingActive =
            instance.occupied && is_active(diagnostics, instance.view.binding);
        const bool bindingRetained =
            instance.occupied && state::activity::binding_matches(instance.view.binding);
        // Rebuild a stale SDK/world view only while the exact client owner remains live.
        if (instance.occupied && bindingActive && bindingRetained && !still_exact(instance)
            && rebind_instance(instance)) {
            log_line(core::log::Level::debug, &instance, "rebind", "generation");
            continue;
        }
        if (instance.occupied && (!bindingActive || !still_exact(instance))) {
            log_line(core::log::Level::info, &instance, "close", "stale_generation");
            // Accepted mission inputs belong to the exact SessionBinding, not one ActivityClient
            // generation or one temporary link outage. Clear only after State replaces the exact
            // session generation; otherwise reattach must finish every already-accepted row.
            clear_instance(instance, !bindingRetained);
        }
    }
    for (RuntimeInstance& instance : g_instances) {
        if (instance.occupied) {
            push_session_roster_edges(instance);
        }
    }
    std::array<state::activity::SessionRosterRow, state::activity::kSessionCapacity> roster{};
    std::size_t rosterCount = 0;
    static_cast<void>(state::activity::snapshot_session_roster(roster, rosterCount));
    const sdk::Snapshot catalog = sdk::snapshot();
    publish_fireteam_life(now);
    for (std::size_t index = 0; index < diagnostics.instanceCount; ++index) {
        if (diagnostics.instances[index].active) {
            attach_instance(
                diagnostics.instances[index], {roster.data(), rosterCount}, catalog, now);
        }
    }
}

/** Advances fresh programs only when their declared state roster has reached transport output. */
void service_pending_starts(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || !instance.startPending
            || instance.programStatus != ProgramStatus::loaded || instance.missionStarted) {
            continue;
        }
        switch (initial_state_gate(instance)) {
        case InitialStateGate::pending:
            break;
        case InitialStateGate::failed:
            instance.startPending = false;
            break;
        case InitialStateGate::ready:
            static_cast<void>(start_program(instance, now));
            break;
        }
    }
}

} // namespace sunrise::server::activity::mission
