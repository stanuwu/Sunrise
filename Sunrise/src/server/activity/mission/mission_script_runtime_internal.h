#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/activity_sdk/generated_world/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../host_runtime.h"
#include "mission_script_actor_path_sense.h"
#include "mission_script_ghost_sense.h"
#include "mission_script_object_sense.h"
#include "mission_script_player_sense.h"
#include "mission_script_runtime.h"
#include "mission_script_squad_sense.h"
#include "mission_script_vm.h"

// What the seven mission-runtime translation units share: the instance table and service slice,
// the attach pipeline, the two Host feeds and the VM callback, the panel rows, the delivery state
// machine, the intent fan-out, and the Sense and host-state edges.

namespace sunrise::server::activity::mission {

namespace sdk = state::activity_sdk;
namespace generated = state::activity_sdk::generated_world;
namespace format = state::activity_sdk::format;
namespace mission_state = state::activity::mission;

enum class ProgramStatus : std::uint8_t {
    none,
    loaded,
    missing,
    fileError,
    sourceTooLarge,
    programError,
};

enum class DeliveryStage : std::uint8_t {
    idle,
    awaitingHostCommit,
    awaitingTransport,
    awaitingCancel,
};

/** Stable result classes bound repeated attach logs. */
enum class AttachResult : std::uint8_t {
    none,
    catalogUnavailable,
    noActivityLink,
    sdkStatus,
    generatedWorldStatus,
    capacity,
    noScript,
    scriptFileError,
    sourceTooLarge,
    programError,
    ready,
};

/** Copies text into a fixed buffer, truncating it, and always leaves it terminated. */
template <std::size_t Capacity>
void copy_text(std::array<char, Capacity>& output, std::string_view value) noexcept {
    output = {};
    const std::size_t length = (std::min)(value.size(), output.size() - 1);
    std::copy_n(value.data(), length, output.data());
}

/** Watched trigger volumes retained per instance. */
constexpr std::size_t kTriggerOccupancyCapacity = 32;
/** Watched squads retained per instance. A social destination places well over a hundred. */
constexpr std::size_t kSquadObservationCapacity = 160;
/** Watched authored scenes retained per instance. */
constexpr std::size_t kSceneObservationCapacity = 32;
/** Watched objective sensors retained per instance. */
constexpr std::size_t kObjectiveObservationCapacity = 32;
static_assert(kSquadObjectiveGroupCount == host::kSquadObjectiveGroupCount);
/** Watched Ghost links, damage monitors, interactable objects and named actors per instance. */
constexpr std::size_t kGhostObservationCapacity = 8;
constexpr std::size_t kDamageObservationCapacity = 8;
constexpr std::size_t kObjectInteractionObservationCapacity = 64;
constexpr std::size_t kActorPathObservationCapacity = 64;
/** One objective sensor carries this many objective blocks. */
constexpr std::size_t kObjectiveCapacity = 24;
/** One objective block carries this many task counters. */
constexpr std::size_t kObjectiveTaskCapacity = 24;
/** How long a committed Host output may take to reach transport before the delivery faults. */
constexpr std::uint64_t kTransportTimeoutMs = 15'000;
/** How long an enqueued Host output may take to reach the reducer before the delivery retries. */
constexpr std::uint64_t kHostCommitTimeoutMs = 2'000;

/** Last occupancy seen for one watched volume, so only a change raises an event. */
struct TriggerOccupancy final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool occupied{};
    bool used{};
};

/** Last movement and delivery level seen for one named actor, so only a change raises an event. */
struct ActorPathObservation final {
    ActorPathLevel level{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last health, shield and revision seen for one damage monitor. */
struct DamageObservation final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    std::int32_t revision{};
    float health{-1.0F};
    float shield{-1.0F};
    bool used{};
};

/** Last object and interaction level seen for one interactable object. */
struct ObjectInteractionObservation final {
    ObjectInteractionLevel level{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last Ghost-link level seen for one sensor. */
struct GhostObservation final {
    GhostLevel level{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last squad counters seen for one watched object, so only a change raises an event. */
struct SquadObservation final {
    SquadObjectiveCosts objectiveCosts{};
    std::array<std::int32_t, host::kSquadSlotCapacity> slotCounts{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::int32_t aliveCount{};
    std::uint16_t slotIndex{};
    std::uint8_t slotCountLength{};
    bool removalFlag{};
    bool used{};
};
/** Last completion latch seen for one watched authored scene, so only the edge raises an event. */
struct SceneObservation final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool completed{};
    bool used{};
};

/** Last task counters seen for one watched objective sensor, so only a rise raises an event. */
struct ObjectiveObservation final {
    std::array<std::array<std::uint8_t, kObjectiveTaskCapacity>, kObjectiveCapacity> counters{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last peer session seen sharing this destination, so only a change raises an event. */
struct SessionRosterWatch final {
    std::uint64_t sessionId{};
    std::uint64_t createdRevision{};
    std::uint64_t memberKey{};
    std::uint64_t joinIdentity{};
    bool used{};
};

/** Everything one bound mission program owns: its VM, views, delivery state and counters. */
struct RuntimeInstance final {
    lua_vm::Vm vm{};
    sdk::BoundView view{};
    generated::GeneratedWorldView worldView{};
    lua_vm::ProgramIdentity identity{};
    mission_state::ProgramKey programKey{};
    std::array<char, 16> lastVmStage{};
    std::array<char, 32> lastVmStatus{};
    std::uint64_t eventsSeen{};
    std::uint64_t eventsCommitted{};
    std::uint64_t intentsTransportStaged{};
    std::uint64_t lastEventSequence{};
    std::uint64_t lastLoggedRevision{};
    std::uint64_t lastMissionSequence{};
    std::uint64_t missionStateRevision{};
    std::uint32_t missionPhase{};
    std::uint64_t activityStateRevision{};
    std::uint64_t durableIntentSequence{};
    std::uint64_t durableHostOutputRevision{};
    std::uint64_t expectedScriptableRevision{};
    std::uint64_t deliveryDeadline{};
    std::uint64_t firstIntentAttempt{};
    std::uint64_t nextIntentAttempt{};
    std::uint64_t firstStartAttempt{};
    std::uint64_t nextStartAttempt{};
    host::Event pendingTimerEvent{};
    std::uint64_t firstTimerAttempt{};
    std::uint64_t nextTimerAttempt{};
    std::array<TriggerOccupancy, kTriggerOccupancyCapacity> triggerOccupancy{};
    std::array<SquadObservation, kSquadObservationCapacity> squadObservations{};
    std::array<GhostObservation, kGhostObservationCapacity> ghostObservations{};
    std::array<DamageObservation, kDamageObservationCapacity> damageObservations{};
    std::array<ObjectInteractionObservation, kObjectInteractionObservationCapacity>
        objectInteractionObservations{};
    std::array<ActorPathObservation, kActorPathObservationCapacity> actorPathObservations{};
    std::array<SceneObservation, kSceneObservationCapacity> sceneObservations{};
    std::array<ObjectiveObservation, kObjectiveObservationCapacity> objectiveObservations{};
    // The table is exactly as large as the session table, so it can never overflow.
    std::array<SessionRosterWatch, state::activity::kSessionCapacity> sessionRoster{};
    /** Participation levels of the sixteen player slots, for the client generation below. */
    std::array<PlayerLifeObservation, kParticipationSlotCount> playerLife{};
    std::uint64_t playerLifeGeneration{};
    /** Last published party life counts. Published once, then only on a change. */
    FireteamLife lastFireteamLife{};
    bool fireteamLifePublished{};
    /** Dynamically sized host-state reports waiting for this script, in arrival order. */
    std::vector<host::Event> scriptEvents{};
    std::uint64_t firstScriptEventAttempt{};
    std::uint64_t nextScriptEventAttempt{};
    std::uint32_t intentAttempts{};
    std::uint32_t startAttempts{};
    std::uint32_t timerAttempts{};
    std::uint32_t scriptEventAttempts{};
    std::size_t durablePendingIntentCount{};
    std::size_t scriptEventRead{};
    std::uint16_t lastIntentStatus{(std::numeric_limits<std::uint16_t>::max)()};
    std::int32_t initialStateRegion{-1};
    /** Last client-reported activity region used to select state-local incoming references. */
    std::int32_t activeRegion{-1};
    ProgramStatus programStatus{ProgramStatus::none};
    DeliveryStage deliveryStage{DeliveryStage::idle};
    /** The player key the bound link's message 5 binds, read at attach. */
    std::uint64_t playerKey{};
    bool publicTarget{};
    bool missionStateBound{};
    bool missionStarted{};
    bool missionStateFaulted{};
    bool initialStateDeclared{};
    bool initialStateSelected{};
    bool startPending{};
    bool timerPending{};
    /** Set after the first roster read, so an attach never replays every existing peer. */
    bool sessionRosterObserved{};
    /** This VM reattached to its current session, so its entry is on_load, not on_start. */
    bool missionReattached{};
    bool occupied{};
};

/** Writes one bounded mission-script diagnostic line. Fields are key=value, error is free text. */
void log_line(core::log::Level level,
              const RuntimeInstance* instance,
              std::string_view stage,
              std::string_view result,
              std::string_view fields = {},
              std::string_view error = {}) noexcept;
/** Appends one host-state event for the script. */
void push_script_event(RuntimeInstance& instance, const host::Event& event) noexcept;
/** Raises a fireteam event on each private instance whose party life counts changed. */
void publish_fireteam_life(std::uint64_t now) noexcept;
/** Merges the type-13 participation records of one Sense snapshot into the instance. */
void observe_player_life(RuntimeInstance& instance,
                         const host::SenseObservationSnapshot& sense) noexcept;

/** Raises one event per watched trigger volume whose occupancy changed. */
void push_trigger_edges(RuntimeInstance& instance,
                        const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one dedicated type-31 edge from a decoded schema-0x8080879F msg-19 payload. */
void push_player_trigger(RuntimeInstance& instance, const host::Event& incident) noexcept;
/** Raises one exact Type-6 start/finish edge from a decoded schema-0x808087BF msg-19 payload. */
void push_cinematic(RuntimeInstance& instance, const host::Event& incident) noexcept;
/** Raises one event per named actor whose movement or delivery level changed. */
void push_actor_path_edges(RuntimeInstance& instance,
                           const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one event per damage monitor whose health, shield or revision changed. */
void push_damage_edges(RuntimeInstance& instance,
                       const host::SenseObservationSnapshot& sense) noexcept;
/** Raises object state and accepted interaction events per interactable object. */
void push_object_interaction_edges(RuntimeInstance& instance,
                                   const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one event per Ghost link whose level changed. */
void push_ghost_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept;
/** Raises the squad state, spawn and death events derived from one msg 6 body. */
void push_squad_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one event per watched authored scene that latched complete. */
void push_scene_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one event per watched objective task counter that rose. */
void push_objective_edges(RuntimeInstance& instance,
                          const host::SenseObservationSnapshot& sense) noexcept;
/** Reports one committed phase change to the script. */
void queue_phase_entered(RuntimeInstance& instance, std::uint32_t previousPhase) noexcept;
/** @return The identity every host-state edge that is not a Sense edge carries. */
[[nodiscard]] host::Event state_edge_event(const RuntimeInstance& instance) noexcept;
/** Raises one event per peer session that appeared or left this instance's destination. */
void push_session_roster_edges(RuntimeInstance& instance,
                               std::span<const state::activity::SessionRosterRow> roster) noexcept;

// The runtime unit owns these. The delivery state machine calls into them.

/** Retires every queued mission event that belongs to one binding. */
void clear_pending_events(const state::activity::SessionBinding& binding) noexcept;
/** Copies one committed authoritative snapshot into the runtime's exact compare baseline. */
void accept_mission_state(RuntimeInstance& instance,
                          const mission_state::Snapshot& snapshot) noexcept;
/** Marks the already-faulted VM in durable State when its exact compare still matches. */
void persist_mission_fault(RuntimeInstance& instance) noexcept;
/** Faults both the VM and the exact server-owned mission record. */
void fault_instance(RuntimeInstance& instance, std::string_view reason) noexcept;

// The delivery unit owns these. The runtime unit's service slice calls into them.

/** @return now plus delay, saturated at the maximum instead of wrapping. */
[[nodiscard]] std::uint64_t deadline_after(std::uint64_t now, std::uint64_t delay) noexcept;
/** Retires the head event after its single delivery attempt. */
void retire_script_event(RuntimeInstance& instance) noexcept;
/** Advances the delivery stage from one Host delivery lifecycle event. */
void observe_delivery_event(RuntimeInstance& instance,
                            const host::Event& event,
                            std::uint64_t now) noexcept;
/** Reconciles an output assigned before a terminal program fault. */
void reconcile_terminal_delivery(RuntimeInstance& instance) noexcept;

// The delivery unit owns these too. The intent fan-out drives the state machine through them.

/** Releases an exact unstaged Host revision while retaining the durable intent. */
[[nodiscard]] bool release_delivery_state(RuntimeInstance& instance) noexcept;
/** Returns the instance to the idle stage and clears delivery timing. */
void clear_delivery(RuntimeInstance& instance) noexcept;
/** Retires one successfully applied local effect without assigning a Host output revision. */
[[nodiscard]] bool complete_local_effect(RuntimeInstance& instance,
                                         std::string_view result) noexcept;
/** Faults the program and abandons the delivery. */
void fault_delivery(RuntimeInstance& instance,
                    std::string_view result,
                    std::string_view reason) noexcept;
/** Reports one refused request to the script and keeps the program running. */
void refuse_delivery(RuntimeInstance& instance,
                     std::string_view result,
                     std::string_view reason,
                     host::EffectOutcome outcome) noexcept;
/** Dedup keys for report_intent_status. Each pending wait owns one value in its own range. */
inline constexpr std::uint16_t kIntentStatusCancelPending = 0x2FF;
inline constexpr std::uint16_t kIntentStatusSceneOutputBusy = 0x301;
inline constexpr std::uint16_t kIntentStatusStateTransitionPending = 0x302;
inline constexpr std::uint16_t kIntentStatusSceneLeasePending = 0x303;
/** Logs one adapter status, and only when it differs from the last one logged. */
void report_intent_status(RuntimeInstance& instance,
                          std::uint16_t status,
                          std::string_view name) noexcept;
/** @return Whether the intent has been in delivery longer than its lifetime allows. */
[[nodiscard]] bool intent_lifetime_expired(const RuntimeInstance& instance,
                                           std::uint64_t now) noexcept;
/** @return Whether an exact transport stage was found, so the delivery completed here. */
[[nodiscard]] bool reconcile_transport_stage(RuntimeInstance& instance) noexcept;
/** @return Whether an expired delivery was completed or left owned, so the caller must stop. */
[[nodiscard]] bool reconcile_expired_delivery(RuntimeInstance& instance) noexcept;
/** @return Whether a stage deadline fired, so no further delivery work is owed this tick. */
[[nodiscard]] bool service_delivery_timeout(RuntimeInstance& instance, std::uint64_t now) noexcept;

// The dispatch unit owns this. The runtime unit's service slice calls it.

/** Raises one queued intent, or advances the delivery already in flight. */
void dispatch_intent(RuntimeInstance& instance, std::uint64_t now) noexcept;

// The runtime unit owns the instance table too. Every other unit reaches it through these.

extern std::array<RuntimeInstance, host::kInstanceCapacity> g_instances;

/** @return The open slot for one exact binding, or null. */
[[nodiscard]] RuntimeInstance*
find_instance(const state::activity::SessionBinding& binding) noexcept;
/** @return One unused slot, or null when every slot is open. */
[[nodiscard]] RuntimeInstance* free_instance() noexcept;
/** Frees one slot; its queued events are retired unless the caller keeps them for a reattach. */
void clear_instance(RuntimeInstance& instance, bool clearPending = true) noexcept;
/** Retains the last VM stage and status shown on the panel. */
void note_vm_status(RuntimeInstance& instance,
                    std::string_view stage,
                    std::string_view status) noexcept;
/** Commits the VM's phase/revision/start transaction into exact server-owned State. */
[[nodiscard]] bool commit_mission_state(RuntimeInstance& instance,
                                        bool started,
                                        std::uint64_t nextInputSequence) noexcept;

// The attach unit owns these. The service slice and the lifecycle entry points call into them.

/** Resolves the script root and the SDK Lua search path. Logs its own refusal. */
[[nodiscard]] bool resolve_script_paths() noexcept;
/** Clears the script buffer, both paths and every reload authorization. */
void clear_script_paths() noexcept;
/** @return False when no authorization slot is free, so the reload cannot replace this program. */
[[nodiscard]] bool authorize_reload(const RuntimeInstance& instance) noexcept;
/** Drops slots that no longer match, publishes the roster, and attaches active host instances. */
void synchronize_instances(std::uint64_t now) noexcept;
/** Binds one host instance to a free slot once its link, SDK view and world view all resolve. */
void attach_instance(const host::InstanceSnapshot& hostInstance,
                     const sdk::Snapshot& catalog,
                     std::uint64_t now) noexcept;
/** Advances fresh programs only when their declared state roster has reached transport output. */
void service_pending_starts(std::uint64_t now) noexcept;

// The diagnostics unit owns these. The attach unit and the panel snapshot call into them.

/** @return Stable panel-facing class for one retained attach result. */
[[nodiscard]] const char* attach_result_name(AttachResult value) noexcept;
/**
 * Reports an attach result when it changes for the binding.
 * @param name Stable result token written to the log.
 * @param activityRow Zero-based SDK row, or the absent sentinel before one resolves.
 */
void report_attach_result(
    const state::activity::SessionBinding& binding,
    AttachResult result,
    std::string_view name,
    std::uint32_t activityRow = format::kAbsentIndex,
    sdk::Status sdkStatus = sdk::Status::notReady,
    generated::BindStatus generatedWorldStatus = generated::BindStatus::invalidBoundView) noexcept;
/** @return True when the Host still reports this binding as active. */
[[nodiscard]] bool is_active(const host::DiagnosticsSnapshot& diagnostics,
                             const state::activity::SessionBinding& binding) noexcept;
/** Drops results after their exact bindings leave the active Host set. */
void retire_attach_diagnostics(const host::DiagnosticsSnapshot& diagnostics) noexcept;
/** Clears every retained attach result. */
void clear_attach_diagnostics() noexcept;
/** Copies one instance and its VM counters into the panel-facing diagnostics row. */
void copy_diagnostics(const RuntimeInstance& instance, InstanceDiagnostics& output) noexcept;
/** Copies every retained attach result into the panel-facing snapshot. */
void snapshot_attach_diagnostics(DiagnosticsSnapshot& output) noexcept;

// The feed unit owns these. The service slice and the lifecycle entry points call into them.

/** Retires every queued mission event that belongs to one binding. */
void clear_all_pending_events() noexcept;
/** Keeps accepted values but removes every VM- and ActivityClient-generation-local decision. */
void reset_pending_events_for_reattach(const state::activity::SessionBinding& binding) noexcept;
/** Retires rows as soon as authoritative State no longer owns their exact binding. */
void retire_unbound_pending_events() noexcept;
/** @return Queued rows still owed to one exact binding. */
[[nodiscard]] std::size_t
pending_event_count(const state::activity::SessionBinding& binding) noexcept;
/** @return True when one queued accepted row is still owed to this instance. */
[[nodiscard]] bool has_pending_host_input(const RuntimeInstance& instance) noexcept;
/** Points both Host cursors at the current feed heads and replays retained accepted inputs. */
void reset_feed_cursors() noexcept;
/** Clears both Host cursors. */
void clear_feed_cursors() noexcept;
/** Reads new host events, advances delivery, and queues only the lifecycle rows for scripts. */
void consume_delivery_events(std::uint64_t now) noexcept;
/** Reads the ordered mission-input feed and drains the queue. */
void consume_mission_inputs(std::uint64_t now) noexcept;
/**
 * Runs one event through the VM, commits what it changed, and faults on a script failure.
 * @param sense Values owned by a Sense row, or null.
 * @param clientMessage Envelope snapshot owned by a client-message row, or null.
 * @param firstAttempt True on the first delivery attempt, which is the one that counts it.
 * @return The VM call status; `inactive` when no callback could take the event.
 */
[[nodiscard]] lua_vm::CallStatus dispatch_event(RuntimeInstance& instance,
                                                const host::Event& event,
                                                const host::SenseObservationSnapshot* sense,
                                                const host::ClientMessageSnapshot* clientMessage,
                                                bool firstAttempt,
                                                std::uint64_t now) noexcept;

} // namespace sunrise::server::activity::mission
