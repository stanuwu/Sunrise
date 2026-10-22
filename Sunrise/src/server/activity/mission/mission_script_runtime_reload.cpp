/**
 * The reload mailbox. The panel or the Studio link asks; the server thread answers once, after
 * every program the request names reaches a safe point. Service runs under the runtime lock.
 */

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../../state/mods/mods_runtime.h"
#include "mission_script_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {
namespace {

using ModId = std::array<char, state::mods::kIdCapacity>;
using Message = decltype(ReloadResult::message);

/** Owns the selected mod ids after releasing the mod-list lock. */
struct ReloadScope final {
    std::array<ModId, state::mods::kModCapacity> ids{};
    std::size_t count{};
    bool all{};

    /** Named scopes match complete mod ids, never prefixes. */
    [[nodiscard]] bool includes(std::string_view id) const noexcept {
        if (all) {
            return true;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (std::string_view(ids[index].data()) == id) {
                return true;
            }
        }
        return false;
    }
};

/** A library update also replaces controllers in its loaded transitive dependents. */
[[nodiscard]] bool expand_scope(void* context, const state::mods::View& view) noexcept {
    auto& scope = *static_cast<ReloadScope*>(context);
    bool found = false;
    for (std::size_t index = 0; index < view.loadedCount; ++index) {
        found = found || scope.includes(view.records[index].manifest.id.view());
    }
    if (!found) {
        return false;
    }
    for (std::size_t pass = 0; pass < view.loadedCount; ++pass) {
        const std::size_t before = scope.count;
        for (std::size_t index = 0; index < view.loadedCount; ++index) {
            const auto& manifest = view.records[index].manifest;
            if (scope.includes(manifest.id.view())) {
                continue;
            }
            for (std::size_t dependency = 0; dependency < manifest.dependencyCount; ++dependency) {
                if (scope.includes(manifest.dependencies[dependency].id.view())) {
                    if (scope.count == scope.ids.size()) {
                        return false;
                    }
                    copy_text(scope.ids[scope.count++], manifest.id.view());
                    break;
                }
            }
        }
        if (scope.count == before) {
            break;
        }
    }
    return true;
}

/** The one request the mailbox holds, and the answer to it. */
struct Mailbox final {
    std::uint64_t nextTicket{1};
    std::uint64_t ticket{};
    ModId modId{};
    ReloadRequester requester{};
    bool pending{};
    ReloadResult result{};
};

/** Source and candidate effects remain private until the whole reload can commit. */
struct Target final {
    state::activity::SessionBinding binding{};
    std::vector<char> source{};
    ModId modId{};
    lua_vm::Vm vm{};
    lua_vm::ProgramIdentity identity{};
    mission_state::ProgramKey program{};
    mission_state::Snapshot expected{}, replacement{};
    std::vector<host::Event> events{};
    bool present{};
};

/** The request the server thread is serving and the programs it names. */
struct Work final {
    std::uint64_t ticket{};
    std::array<std::unique_ptr<Target>, host::kInstanceCapacity> targets{};
    std::size_t targetCount{};
};

/** Any thread may ask; only the server thread answers. */
SRWLOCK g_mailboxLock{SRWLOCK_INIT};
Mailbox g_mailbox{};
/** Only the server thread touches this. */
Work g_work{};

/** The caller holds the mailbox exclusively through publication and its answer. */
void store_answer(std::uint64_t ticket,
                  ReloadStatus status,
                  std::uint32_t reopened,
                  std::string_view message) noexcept {
    if (g_mailbox.ticket == ticket && g_mailbox.pending) {
        g_mailbox.pending = false;
        g_mailbox.result.status = status;
        g_mailbox.result.reopened = reopened;
        copy_text(g_mailbox.result.message, message);
    }
}

/** @return The log spelling of one reload status. */
[[nodiscard]] const char* reload_status_name(ReloadStatus status) noexcept {
    switch (status) {
    case ReloadStatus::pending:
        return "pending";
    case ReloadStatus::ok:
        return "ok";
    case ReloadStatus::disabled:
        return "disabled";
    case ReloadStatus::compileError:
        return "compile_error";
    case ReloadStatus::failed:
        return "failed";
    case ReloadStatus::replaced:
        return "replaced";
    case ReloadStatus::cancelled:
        return "cancelled";
    }
    return "unknown";
}

/** Logging runs after releasing the mailbox. */
void log_answer(ReloadStatus status, std::uint32_t reopened, std::string_view message) noexcept {
    std::array<char, 48> fields{};
    const int written = std::snprintf(fields.data(),
                                      fields.size(),
                                      "status=%s reopened=%u",
                                      reload_status_name(status),
                                      reopened);
    log_line(status == ReloadStatus::ok ? core::log::Level::info : core::log::Level::warn,
             nullptr,
             "reload",
             "answered",
             written > 0 ? std::string_view(fields.data()) : std::string_view{},
             message);
}

/** Records the answer unless a newer request already replaced this ticket. */
void answer(std::uint64_t ticket,
            ReloadStatus status,
            std::uint32_t reopened,
            std::string_view message) noexcept {
    AcquireSRWLockExclusive(&g_mailboxLock);
    store_answer(ticket, status, reopened, message);
    ReleaseSRWLockExclusive(&g_mailboxLock);
    log_answer(status, reopened, message);
}

/** @return False when no request is waiting. */
[[nodiscard]] bool take_request(std::uint64_t& ticket, ModId& modId) noexcept {
    AcquireSRWLockShared(&g_mailboxLock);
    const bool pending = g_mailbox.pending;
    ticket = g_mailbox.ticket;
    modId = g_mailbox.modId;
    ReleaseSRWLockShared(&g_mailboxLock);
    return pending;
}

/** @return True when nothing the old program started is still in flight. */
[[nodiscard]] bool at_safe_point(const RuntimeInstance& instance) noexcept {
    // A program that is not running has nothing in flight to lose.
    if (instance.programStatus != ProgramStatus::loaded || instance.missionStateFaulted) {
        return true;
    }
    lua_vm::Snapshot vm{};
    lua_vm::snapshot(instance.vm, vm);
    if (vm.faulted) {
        return true;
    }
    mission_state::InputSequenceSnapshot inputs{};
    // prepare_reload refuses a record with uncommitted inputs.
    const bool inputsSettled =
        !instance.missionStateBound
        || (mission_state::input_sequence_snapshot(instance.view.binding, inputs)
            && inputs.committed == inputs.issued);
    return inputsSettled && instance.deliveryStage == DeliveryStage::idle
           && instance.durablePendingIntentCount == 0 && vm.pendingIntents == 0
           && !instance.timerPending && instance.scriptEventRead >= instance.scriptEvents.size()
           && !has_pending_host_input(instance);
}

/**
 * Names the programs one request reopens and compiles each new controller once.
 * @param scope Mod id; empty names every started program.
 * @param error Receives the first refusal or compiler message.
 * @param failureStatus Receives compileError or failed when the return is false.
 * @return False when a named program cannot be prepared.
 */
[[nodiscard]] bool
collect_targets(std::string_view scope, Message& error, ReloadStatus& failureStatus) noexcept {
    failureStatus = ReloadStatus::failed;
    ReloadScope selected{};
    selected.all = scope.empty();
    if (!selected.all) {
        copy_text(selected.ids[selected.count++], scope);
        if (!state::mods::visit(&expand_scope, &selected)) {
            static_cast<void>(std::snprintf(error.data(),
                                            error.size(),
                                            "mod %.*s is not loaded",
                                            static_cast<int>(scope.size()),
                                            scope.data()));
            return false;
        }
    }
    for (RuntimeInstance& instance : g_instances) {
        // Only a started mission can reload; one that never started keeps its program.
        if (!instance.occupied || !instance.missionStateBound || !instance.missionStarted) {
            continue;
        }
        const format::Activity* const activity = sdk::bound_activity(instance.view);
        std::span<const char> source{};
        ModId newMod{};
        const SourceStatus status =
            activity != nullptr && instance.view.catalog != nullptr
                ? read_source(*instance.view.catalog, *activity, source, newMod)
                : SourceStatus::missing;
        // A mod may take over a controller the base supplied, so the new owner counts too.
        bool inScope =
            selected.includes(instance.identity.modId.data()) || selected.includes(newMod.data());
        for (std::size_t mod = 0; !inScope && mod < selected.count; ++mod) {
            inScope = lua_vm::uses_mod(instance.vm, selected.ids[mod].data());
        }
        if (!inScope) {
            clear_source(source);
            continue;
        }
        if (status != SourceStatus::ready) {
            clear_source(source);
            copy_text(error, "reload source is missing, unreadable or too large");
            return false;
        }
        try {
            auto target = std::make_unique<Target>();
            target->binding = instance.view.binding;
            target->source.assign(source.begin(), source.end());
            target->modId = newMod;
            const auto opened = prepare_reload_program(instance,
                                                       target->source,
                                                       target->modId.data(),
                                                       target->vm,
                                                       target->identity,
                                                       target->program);
            if (opened != lua_vm::OpenStatus::ready) {
                lua_vm::Snapshot diagnostics{};
                lua_vm::snapshot(target->vm, diagnostics);
                copy_text(error,
                          diagnostics.lastError[0] != '\0'
                              ? std::string_view(diagnostics.lastError.data())
                              : lua_vm::status_name(opened));
                failureStatus = opened == lua_vm::OpenStatus::compileError
                                    ? ReloadStatus::compileError
                                    : ReloadStatus::failed;
                clear_source(source);
                return false;
            }
            g_work.targets[g_work.targetCount++] = std::move(target);
        } catch (const std::bad_alloc&) {
            clear_source(source);
            copy_text(error, "reload source could not be retained");
            return false;
        }
        clear_source(source);
    }
    return true;
}

/** @return True when every named program still open is at a safe point. */
[[nodiscard]] bool targets_at_safe_point() noexcept {
    for (std::size_t index = 0; index < g_work.targetCount; ++index) {
        const RuntimeInstance* const instance = find_instance(g_work.targets[index]->binding);
        if (instance != nullptr && !at_safe_point(*instance)) {
            return false;
        }
    }
    return true;
}

/** Opens and runs on_load privately; a refused candidate leaves its running program untouched. */
[[nodiscard]] bool prepare_target(Target& target,
                                  RuntimeInstance& instance,
                                  std::uint64_t now,
                                  Message& failure) noexcept {
    if (!instance.missionStateBound || !instance.missionStarted
        || !mission_state::state_snapshot(target.binding, target.expected)
        || !target.expected.state.programBound || !target.expected.state.started) {
        copy_text(failure, "only started, bound missions can reload; restart this activity");
        return false;
    }
    if (!rebind_reload_program(
            instance, target.source, target.vm, target.identity, target.program)) {
        copy_text(failure, "prepared reload no longer matches the running activity");
        return false;
    }
    const auto& prior = target.expected.state;
    if (prior.variableCount > prior.variables.size() || prior.timerCount > prior.timers.size()
        || !prior.pendingIntents.empty() || !lua_vm::copy_inputs(instance.vm, target.vm)
        || !lua_vm::restore_state(target.vm,
                                  prior.phase,
                                  prior.revision,
                                  std::span(prior.variables).first(prior.variableCount),
                                  std::span(prior.timers).first(prior.timerCount),
                                  prior.nextTimerSequence,
                                  prior.nextIntentKey,
                                  {})) {
        copy_text(failure, "replacement mission state could not be prepared");
        return false;
    }
    lua_vm::publish_attempt(target.vm, prior.attempt);
    if (!lua_vm::publish_device_requests(target.vm, prior.deviceRequests)
        || !publish_device_sources(instance, target.vm)
        || !lua_vm::publish_populations(target.vm, prior.squadPopulations)) {
        copy_text(failure, "replacement observations could not be prepared");
        return false;
    }
    const auto loaded = lua_vm::load(target.vm, now);
    lua_vm::Snapshot diagnostics{};
    lua_vm::snapshot(target.vm, diagnostics);
    if (loaded != lua_vm::CallStatus::committed && loaded != lua_vm::CallStatus::noHandler) {
        copy_text(failure,
                  diagnostics.lastError[0] != '\0' ? std::string_view(diagnostics.lastError.data())
                                                   : lua_vm::status_name(loaded));
        return false;
    }
    std::vector<mission_state::TypedIntent> intents;
    std::array<mission_state::ScriptVariable, mission_state::kVariableCapacity> variables{};
    std::array<mission_state::MissionTimer, mission_state::kTimerCapacity> timers{};
    std::size_t variableCount = 0, timerCount = 0;
    std::uint64_t nextTimer = 0, nextIntent = 0;
    if (!lua_vm::snapshot_intents(target.vm, intents)
        || !lua_vm::snapshot_durable_state(
            target.vm, variables, variableCount, timers, timerCount, nextTimer, nextIntent)) {
        copy_text(failure, "replacement effects could not be prepared");
        return false;
    }
    const mission_state::CommitCandidate transaction{
        .variables = std::span(variables).first(variableCount),
        .timers = std::span(timers).first(timerCount),
        .pendingIntents = intents,
        .nextRevision = diagnostics.stateRevision,
        .nextInputSequence = prior.inputSequence,
        .nextTimerSequence = nextTimer,
        .nextIntentKey = nextIntent,
        .phase = diagnostics.phase,
        .started = true,
    };
    const auto status = mission_state::prepare_reload(
        target.expected, target.program, transaction, target.replacement);
    if (status != mission_state::Status::ready) {
        copy_text(failure, mission_state::status_name(status));
        return false;
    }
    if (!lua_vm::publish_device_requests(target.vm, target.replacement.state.deviceRequests)
        || !publish_device_sources(instance, target.vm)
        || !lua_vm::publish_populations(target.vm, target.replacement.state.squadPopulations)) {
        copy_text(failure, "replacement effects could not be retained");
        return false;
    }
    if (target.replacement.state.phase != instance.missionPhase
        && lua_vm::handles_event(target.vm, host::EventKind::phaseEntered)) {
        host::Event event{};
        event.attemptGeneration = target.replacement.state.attempt.generation;
        event.binding = target.binding;
        event.sequence = target.replacement.state.revision;
        event.sourceGeneration = instance.view.activityClientGeneration;
        event.missionSequence = target.replacement.state.inputSequence;
        event.stateRevision = target.replacement.state.revision;
        event.missionPhase = target.replacement.state.phase;
        event.previousMissionPhase = instance.missionPhase;
        event.kind = host::EventKind::phaseEntered;
        try {
            target.events.push_back(event);
        } catch (const std::bad_alloc&) {
            copy_text(failure, "replacement phase event could not be prepared");
            return false;
        }
    }
    target.present = true;
    return true;
}

/** Publishes all candidate State together, then adopts VMs without reopening or allocating. */
void reopen_targets(std::uint64_t ticket, std::uint64_t now) noexcept {
    Message failure{};
    std::array<mission_state::ReloadCommit, host::kInstanceCapacity> commits{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < g_work.targetCount; ++index) {
        Target& target = *g_work.targets[index];
        RuntimeInstance* const instance = find_instance(target.binding);
        if (instance != nullptr) {
            if (!prepare_target(target, *instance, now, failure)) {
                answer(ticket, ReloadStatus::failed, 0, failure.data());
                return;
            }
            commits[count++] = {target.binding, &target.expected, &target.replacement};
        }
    }
    AcquireSRWLockExclusive(&g_mailboxLock);
    if (g_mailbox.ticket != ticket || !g_mailbox.pending) {
        ReleaseSRWLockExclusive(&g_mailboxLock);
        return;
    }
    if (g_mailbox.requester != nullptr && !g_mailbox.requester(ticket)) {
        store_answer(
            ticket, ReloadStatus::cancelled, 0, "reload requester left before publication");
        ReleaseSRWLockExclusive(&g_mailboxLock);
        log_answer(ReloadStatus::cancelled, 0, "reload requester left before publication");
        return;
    }
    const auto committed = mission_state::commit_reloads(std::span(commits).first(count));
    if (committed != mission_state::Status::ready) {
        store_answer(ticket, ReloadStatus::failed, 0, mission_state::status_name(committed));
        ReleaseSRWLockExclusive(&g_mailboxLock);
        log_answer(ReloadStatus::failed, 0, mission_state::status_name(committed));
        return;
    }
    std::uint32_t reopened = 0;
    for (std::size_t index = 0; index < g_work.targetCount; ++index) {
        Target& target = *g_work.targets[index];
        RuntimeInstance* const instance = find_instance(target.binding);
        if (!target.present || instance == nullptr) {
            continue;
        }
        // The runtime lock excludes callbacks; both states completed their last protected call.
        static_cast<void>(lua_vm::exchange(instance->vm, target.vm));
        instance->identity = target.identity;
        instance->programKey = target.program;
        instance->programStatus = ProgramStatus::loaded;
        instance->missionReattached = true;
        instance->startPending = false;
        if (target.expected.state.faulted) {
            clear_pending_events(target.binding);
            instance->timerPending = false;
            instance->pendingTimerEvent = {};
            instance->deliveryStage = DeliveryStage::idle;
        }
        accept_compare_fields(*instance, target.replacement);
        instance->lastLoggedRevision = instance->missionStateRevision;
        instance->scriptEvents.swap(target.events);
        instance->scriptEventRead = 0;
        note_vm_status(*instance, "load", "committed");
        ++reopened;
    }
    Message message{};
    if (g_work.targetCount == 0) {
        copy_text(message, "no started mission uses this script");
    } else {
        static_cast<void>(
            std::snprintf(message.data(), message.size(), "reopened %u programs", reopened));
    }
    store_answer(ticket, ReloadStatus::ok, reopened, message.data());
    ReleaseSRWLockExclusive(&g_mailboxLock);
    log_answer(ReloadStatus::ok, reopened, message.data());
}

} // namespace

/** A newer request replaces an unanswered one, so the mailbox holds one request at most. */
std::uint64_t request_reload(std::string_view modId, ReloadRequester requester) noexcept {
    const bool ready = runtime_ready();
    AcquireSRWLockExclusive(&g_mailboxLock);
    const std::uint64_t ticket = g_mailbox.nextTicket++;
    g_mailbox.ticket = ticket;
    copy_text(g_mailbox.modId, modId);
    g_mailbox.requester = requester;
    g_mailbox.pending = ready;
    g_mailbox.result = {};
    if (!ready) {
        g_mailbox.result.status = ReloadStatus::disabled;
        copy_text(g_mailbox.result.message, "mission scripting is off");
    }
    ReleaseSRWLockExclusive(&g_mailboxLock);
    return ticket;
}

/** Cancellation and publication share one lock, so a committed reload cannot be cancelled. */
bool cancel_reload(std::uint64_t ticket) noexcept {
    AcquireSRWLockExclusive(&g_mailboxLock);
    const bool pending = ticket != 0 && g_mailbox.ticket == ticket && g_mailbox.pending;
    if (pending) {
        store_answer(ticket, ReloadStatus::cancelled, 0, "reload request was cancelled");
    }
    ReleaseSRWLockExclusive(&g_mailboxLock);
    return pending;
}

/** A ticket a newer request replaced answers `replaced`, never the newer request's result. */
ReloadResult reload_result(std::uint64_t ticket) noexcept {
    ReloadResult result{};
    AcquireSRWLockShared(&g_mailboxLock);
    if (ticket != g_mailbox.ticket) {
        result.status = ReloadStatus::replaced;
    } else if (!g_mailbox.pending) {
        result = g_mailbox.result;
    }
    ReleaseSRWLockShared(&g_mailboxLock);
    return result;
}

/**
 * Prepares the requested programs, waits for safe points, then commits their replacements.
 * @param now Service tick the reattach measures its deadlines from.
 */
void service_reload(std::uint64_t now) noexcept {
    std::uint64_t ticket = 0;
    ModId modId{};
    if (!take_request(ticket, modId)) {
        g_work = {};
        return;
    }
    if (g_work.ticket != ticket) {
        g_work = {};
        g_work.ticket = ticket;
        Message error{};
        ReloadStatus failureStatus{};
        if (!collect_targets(std::string_view(modId.data()), error, failureStatus)) {
            answer(ticket, failureStatus, 0, error.data());
            g_work = {};
            return;
        }
    }
    if (!targets_at_safe_point()) {
        return;
    }
    reopen_targets(ticket, now);
    g_work = {};
}

void clear_reload_mailbox() noexcept {
    AcquireSRWLockExclusive(&g_mailboxLock);
    const std::uint64_t nextTicket = g_mailbox.nextTicket;
    g_mailbox = {};
    g_mailbox.nextTicket = nextTicket;
    ReleaseSRWLockExclusive(&g_mailboxLock);
    g_work = {};
}

} // namespace sunrise::server::activity::mission
