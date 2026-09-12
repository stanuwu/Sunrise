#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/activity/definition.h"

namespace sunrise::server::activity::mission {

/** One bounded, value-owned diagnostic row for an attached mission program. */
struct InstanceDiagnostics final {
    state::activity::SessionBinding binding{};
    std::array<char, 64> activityId{};
    std::array<char, 24> programStatus{};
    std::array<char, 32> deliveryStage{};
    std::array<char, 16> lastVmStage{};
    std::array<char, 32> lastVmStatus{};
    std::array<char, 256> lastVmError{};
    std::uint64_t eventsSeen{};
    std::uint64_t eventsCommitted{};
    std::uint64_t lastEventSequence{};
    std::uint64_t lastMissionSequence{};
    std::uint64_t vmStateRevision{};
    std::uint64_t missionStateRevision{};
    std::uint64_t activityStateRevision{};
    std::uint64_t vmCallbacks{};
    std::uint64_t vmCommittedCallbacks{};
    std::uint64_t vmRefusedCallbacks{};
    std::uint64_t intentsTransportStaged{};
    std::uint64_t expectedScriptableRevision{};
    std::uint32_t activityRow{};
    std::uint32_t phase{};
    std::uint32_t intentAttempts{};
    std::uint32_t startAttempts{};
    std::size_t pendingIntents{};
    std::size_t pendingEvents{};
    bool publicTarget{};
    bool missionStateBound{};
    bool missionStarted{};
    bool startPending{};
    bool vmActive{};
    bool vmFaulted{};
};

/** One bounded, value-owned result from attaching Mission Lua to an exact Host generation. */
struct AttachDiagnostics final {
    state::activity::SessionBinding binding{};
    /** Stable attach-result class, such as `ready`, `no_activity_link`, or `sdk_status`. */
    std::array<char, 32> result{};
    /** Exact refusal detail; SDK failures carry their stable SDK status name here. */
    std::array<char, 32> detail{};
    /** One-based SDK activity row when resolution reached an exact activity. */
    std::uint32_t activityRow{};
    bool hasActivityRow{};
};

/** One fixed-capacity copy of all attached mission-program diagnostics. */
struct DiagnosticsSnapshot final {
    std::array<InstanceDiagnostics, state::activity::kSessionCapacity> instances{};
    /** One row per Host generation plus one possible runtime-capacity refusal. */
    std::array<AttachDiagnostics, state::activity::kSessionCapacity + 1> attaches{};
    std::size_t instanceCount{};
    std::size_t attachCount{};
    bool enabled{};
    bool pathReady{};
};

/** Generation-pinned actor command accepted by a gameplay policy provider. */
struct ActorCommandPolicyRequest final {
    state::activity::SessionBinding binding{};
    std::array<std::byte, 32> sdkBuildSha256{};
    std::uint32_t squadRow{};
    std::uint32_t commandRow{};
    std::uint32_t commandSelector{};
    std::int32_t value{};
};

enum class ActorCommandPolicyStatus : std::uint8_t {
    queued,
    unavailable,
    refused,
};

using ActorCommandPolicy = ActorCommandPolicyStatus (*)(
    const void* context, const ActorCommandPolicyRequest& request) noexcept;

/** Installs the gameplay-owned policy callback before mission service starts. */
void install_actor_command_policy(const void* context, ActorCommandPolicy policy) noexcept;

/** Publishes accepted gameplay damage only to the mission's matching source generation. */
void report_squad_provoked(const state::activity::SessionBinding& binding,
                           std::uint64_t sourceGeneration,
                           std::uint32_t registryKey,
                           std::uint16_t slotIndex) noexcept;

/** Starts the optional, off-by-default server mission-script manager. */
void initialize() noexcept;
/** Writes the readable authored controller filename for one 1-based SDK activity row. */
[[nodiscard]] bool controller_file_name(std::uint32_t oneBasedActivityRow,
                                        std::span<char> output) noexcept;
/** Runs one bounded event/reducer/output slice after Activity Host ingress. */
void service(std::uint64_t now) noexcept;
/** Probes the mission script for the activity to apply its initial_state slice-set override. */
void apply_script_initial_state_override(state::activity::destination::DestinationSelection& selection) noexcept;
/** Copies mission-program state without invoking Lua or changing delivery state. */
void snapshot(DiagnosticsSnapshot& output) noexcept;
/** Closes attached programs and reattaches them from disk on the next service slice. */
[[nodiscard]] bool reload() noexcept;
/** Closes every per-activity Lua state and clears its event cursor. */
void shutdown() noexcept;

} // namespace sunrise::server::activity::mission
