/**
 * Retained attach results and the panel-facing rows built from them.
 * Every function here runs under the mission runtime lock its caller already holds.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/activity/runtime.h"
#include "../host_runtime.h"
#include "mission_script_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {
namespace {

/** One extra slot retains a capacity refusal beyond all runtime slots. */
constexpr std::size_t kAttachDiagnosticCapacity = host::kInstanceCapacity + 1;

/** One binding's last attach result limits repeated gate logs. */
struct AttachDiagnostic final {
    state::activity::SessionBinding binding{};
    sdk::Status sdkStatus{sdk::Status::notReady};
    generated_world::BindStatus generatedWorldStatus{generated_world::BindStatus::invalidBoundView};
    std::uint32_t activityRow{format::kAbsentIndex};
    std::array<char, kControllerFileCapacity> controllerFile{};
    AttachResult result{AttachResult::none};
    bool occupied{};
};

std::array<AttachDiagnostic, kAttachDiagnosticCapacity> g_attachDiagnostics{};

/** Stable lowercase log token for one program status. */
[[nodiscard]] const char* program_status_name(ProgramStatus value) noexcept {
    switch (value) {
    case ProgramStatus::none:
        return "none";
    case ProgramStatus::loaded:
        return "loaded";
    case ProgramStatus::missing:
        return "missing";
    case ProgramStatus::fileError:
        return "file_error";
    case ProgramStatus::sourceTooLarge:
        return "source_too_large";
    case ProgramStatus::programError:
        return "program_error";
    }
    return "unknown";
}

/** Stable lowercase log token for one delivery stage. */
[[nodiscard]] const char* delivery_stage_name(DeliveryStage value) noexcept {
    switch (value) {
    case DeliveryStage::idle:
        return "idle";
    case DeliveryStage::awaitingHostCommit:
        return "awaiting_host_commit";
    case DeliveryStage::awaitingTransport:
        return "awaiting_transport";
    case DeliveryStage::awaitingCancel:
        return "awaiting_cancel";
    }
    return "unknown";
}

/** Emits one attach row with only known identity fields. */
void log_attach_line(core::log::Level level,
                     const state::activity::SessionBinding& binding,
                     std::string_view result,
                     std::uint32_t activityRow = format::kAbsentIndex) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    // BoundView rows are zero-based; every other Mission diagnostic presents ProgramIdentity's
    // one-based row. Keep the absent sentinel out of the log instead of incrementing it.
    const int written =
        activityRow == format::kAbsentIndex
            ? std::snprintf(line.data(),
                            line.size(),
                            "ev=mission_script stage=attach result=%.*s session=%llu",
                            static_cast<int>(result.size()),
                            result.data(),
                            static_cast<unsigned long long>(binding.sessionId))
            : std::snprintf(line.data(),
                            line.size(),
                            "ev=mission_script stage=attach result=%.*s session=%llu "
                            "activity_row=%u",
                            static_cast<int>(result.size()),
                            result.data(),
                            static_cast<unsigned long long>(binding.sessionId),
                            activityRow + 1);
    if (written > 0) {
        core::log::write(
            core::log::Channel::server,
            level,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

/** @return The retained result for one exact binding. */
[[nodiscard]] AttachDiagnostic*
find_attach_diagnostic(const state::activity::SessionBinding& binding) noexcept {
    for (AttachDiagnostic& diagnostic : g_attachDiagnostics) {
        if (diagnostic.occupied && same_binding(diagnostic.binding, binding)) {
            return &diagnostic;
        }
    }
    return nullptr;
}

/** @return One free result slot, or null when every Host slot is represented. */
[[nodiscard]] AttachDiagnostic* free_attach_diagnostic() noexcept {
    for (AttachDiagnostic& diagnostic : g_attachDiagnostics) {
        if (!diagnostic.occupied) {
            return &diagnostic;
        }
    }
    return nullptr;
}

/** Copies one retained attach result without exposing its internal enum or catalog pointers. */
void copy_attach_diagnostics(const AttachDiagnostic& source, AttachDiagnostics& output) noexcept {
    output = {};
    output.binding = source.binding;
    copy_text(output.result, attach_result_name(source.result));
    copy_text(output.detail,
              source.result == AttachResult::sdkStatus ? sdk::status_name(source.sdkStatus)
              : source.result == AttachResult::generatedWorldStatus
                  ? generated_world::status_name(source.generatedWorldStatus)
                  : attach_result_name(source.result));
    if (source.activityRow != format::kAbsentIndex) {
        output.activityRow = source.activityRow + 1;
        output.hasActivityRow = true;
    }
    output.controllerFile = source.controllerFile;
}

} // namespace

/** @return Stable panel-facing class for one retained attach result. */
const char* attach_result_name(AttachResult value) noexcept {
    switch (value) {
    case AttachResult::none:
        return "none";
    case AttachResult::catalogUnavailable:
        return "catalog_unavailable";
    case AttachResult::noActivityLink:
        return "no_activity_link";
    case AttachResult::sdkStatus:
        return "sdk_status";
    case AttachResult::generatedWorldStatus:
        return "generated_world_status";
    case AttachResult::capacity:
        return "capacity";
    case AttachResult::noScript:
        return "no_script";
    case AttachResult::scriptFileError:
        return "script_file_error";
    case AttachResult::sourceTooLarge:
        return "source_too_large";
    case AttachResult::programError:
        return "program_error";
    case AttachResult::ready:
        return "ready";
    }
    return "unknown";
}

/**
 * Reports an attach result when it changes for the binding.
 * @param name Stable result token written to the log.
 * @param view The resolved activity, or null before one resolves.
 */
void report_attach_result(const state::activity::SessionBinding& binding,
                          AttachResult result,
                          std::string_view name,
                          const sdk::BoundView* view,
                          sdk::Status sdkStatus,
                          generated_world::BindStatus generatedWorldStatus) noexcept {
    const std::uint32_t activityRow = view != nullptr ? view->activityRow : format::kAbsentIndex;
    // A row number alone does not say which catalog, installed or mod, it came from.
    std::array<char, kControllerFileCapacity> controllerFile{};
    if (view != nullptr) {
        controller_file(*view, controllerFile);
    }
    AttachDiagnostic* diagnostic = find_attach_diagnostic(binding);
    if (diagnostic != nullptr && diagnostic->result == result
        && diagnostic->activityRow == activityRow && diagnostic->controllerFile == controllerFile
        && (result != AttachResult::sdkStatus || diagnostic->sdkStatus == sdkStatus)
        && (result != AttachResult::generatedWorldStatus
            || diagnostic->generatedWorldStatus == generatedWorldStatus)) {
        return;
    }
    if (diagnostic == nullptr) {
        diagnostic = free_attach_diagnostic();
    }
    if (diagnostic != nullptr) {
        diagnostic->binding = binding;
        diagnostic->sdkStatus = sdkStatus;
        diagnostic->generatedWorldStatus = generatedWorldStatus;
        diagnostic->activityRow = activityRow;
        diagnostic->controllerFile = controllerFile;
        diagnostic->result = result;
        diagnostic->occupied = true;
    }
    log_attach_line(result == AttachResult::ready ? core::log::Level::info : core::log::Level::warn,
                    binding,
                    name,
                    activityRow);
}

/** @return True when the Host still reports this binding as active. */
bool is_active(const host::DiagnosticsSnapshot& diagnostics,
               const state::activity::SessionBinding& binding) noexcept {
    for (std::size_t index = 0; index < diagnostics.instanceCount; ++index) {
        if (diagnostics.instances[index].active
            && same_binding(diagnostics.instances[index].binding, binding)) {
            return true;
        }
    }
    return false;
}

/** Drops results after their exact bindings leave the active Host set. */
void retire_attach_diagnostics(const host::DiagnosticsSnapshot& diagnostics) noexcept {
    for (AttachDiagnostic& diagnostic : g_attachDiagnostics) {
        if (diagnostic.occupied && !is_active(diagnostics, diagnostic.binding)) {
            diagnostic = {};
        }
    }
}

/** Clears every retained attach result. */
void clear_attach_diagnostics() noexcept {
    g_attachDiagnostics = {};
}

/** Copies one instance and its VM counters into the panel-facing diagnostics row. */
void copy_diagnostics(const RuntimeInstance& instance, InstanceDiagnostics& output) noexcept {
    output = {};
    output.binding = instance.view.binding;
    output.activityId = instance.identity.activityId;
    controller_file(instance.view, output.controllerFile);
    copy_text(output.programStatus, program_status_name(instance.programStatus));
    copy_text(output.deliveryStage, delivery_stage_name(instance.deliveryStage));
    output.lastVmStage = instance.lastVmStage;
    output.lastVmStatus = instance.lastVmStatus;
    output.eventsSeen = instance.eventsSeen;
    output.eventsCommitted = instance.eventsCommitted;
    output.lastMissionSequence = instance.lastMissionSequence;
    output.missionStateRevision = instance.missionStateRevision;
    output.activityStateRevision = instance.activityStateRevision;
    output.expectedScriptableRevision = instance.expectedScriptableRevision;
    output.activityRow = instance.identity.activityId[0] != '\0' ? instance.identity.activityRow
                         : instance.view.activityRow == format::kAbsentIndex
                             ? 0
                             : instance.view.activityRow + 1;
    output.intentAttempts = instance.intentAttempts;
    output.startAttempts = instance.startAttempts;
    output.pendingEvents = pending_event_count(instance.view.binding);
    output.missionStateBound = instance.missionStateBound;
    output.missionStarted = instance.missionStarted;

    lua_vm::Snapshot vm{};
    lua_vm::snapshot(instance.vm, vm);
    output.lastVmError = vm.lastError;
    output.vmStateRevision = vm.stateRevision;
    output.vmCallbacks = vm.callbacks;
    output.vmCommittedCallbacks = vm.committedCallbacks;
    output.vmRefusedCallbacks = vm.refusedCallbacks;
    output.intentsTransportStaged = instance.intentsTransportStaged;
    output.phase = vm.phase;
    output.pendingIntents = vm.pendingIntents;
    output.vmActive = vm.active;
    output.vmFaulted = vm.faulted;
}

/** Copies every retained attach result into the panel-facing snapshot. */
void snapshot_attach_diagnostics(DiagnosticsSnapshot& output) noexcept {
    for (const AttachDiagnostic& diagnostic : g_attachDiagnostics) {
        if (diagnostic.occupied && output.attachCount < output.attaches.size()) {
            copy_attach_diagnostics(diagnostic, output.attaches[output.attachCount++]);
        }
    }
}

} // namespace sunrise::server::activity::mission
