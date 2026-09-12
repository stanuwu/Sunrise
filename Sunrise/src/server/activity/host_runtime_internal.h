#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "host_runtime.h"

namespace sunrise::server::activity::host::detail {

/** One queued operator transition. */
struct ControlRequest final {
    state::activity::SessionBinding binding{};
    std::uint8_t lifetimeState{kDefaultLifetimeState};
};

/** One queued operator incident. */
struct IncidentRequest final {
    state::activity::SessionBinding binding{};
    middleware::bap::activity_message::incident::Incident incident{};
};

/** One queued typed ClientRef request; its counter is assigned by the reducer. */
struct ScriptableRequest final {
    middleware::bap::activity_message::sensor_auth_update::AuthoredSceneDependencies
        sceneDependencies{};
    std::uint32_t sceneEventKey{};
    std::uint32_t sequenceHash{};
    state::gameplay::squad_entity_retirement::Eligibility squadRetirement{};
    state::activity::SessionBinding binding{};
    ScriptableTarget target{};
    state::build_data::scenarios::RosterGroup stateLocalRosterGroup{};
    std::array<std::int32_t,
               middleware::bap::activity_message::squad_auth::kMaximumRequestedCountLength>
        requestedCounts{};
    std::array<std::int8_t, 4> squadAuthoredProfile{};
    std::array<std::byte,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideByteCapacity>
        authBody{};
    std::size_t requestedCountLength{};
    std::uint16_t authBitCount{};
    std::uint16_t authByteCount{};
    std::uint16_t dialogueCue{};
    std::optional<std::uint32_t> nameHash{};
    std::uint64_t expectedActivityClientGeneration{};
    /** Exact reserved revision, or zero for an ordinary operator request. */
    std::uint64_t expectedRevision{};
    /** Exact durable Mission State head, or zero for an ordinary operator request. */
    std::uint64_t expectedIntentSequence{};
    /** This body shares the head's reserved revision instead of holding one of its own. */
    bool burstMember{};
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel{};
    float value{};
    std::uint32_t channelHash{};
    std::int32_t entryIndex{};
    middleware::bap::activity_message::squad_auth::Mode squadMode{
        middleware::bap::activity_message::squad_auth::Mode::mode0};
    ScriptableOverrideKind kind{ScriptableOverrideKind::type23};
    /** Activity lifetime state for a lifetime request; ignored by every other kind. */
    std::uint8_t lifetimeState{kDefaultLifetimeState};
    bool snap{};
    bool active{};
};

/** One owned client envelope without a richer typed mission reducer. */
struct ClientMessageMissionInput final {
    state::activity::SessionBinding binding{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t messageType{};
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    std::uint32_t consumedBits{};
    ClientMessageStatus status{ClientMessageStatus::unclassified};
};

/** Input kind retained in the one ordered reducer queue. */
enum class PendingKind : std::uint8_t {
    sense,
    incident,
    clientStateChange,
    entitySlotsRequested,
    clientMessage,
    authControl,
    incidentControl,
    scriptableControl,
    /** Exact queued durable control withdrawn under the Host lock; accounting is already paid. */
    discardedControl,
};

/** One copied input; only the field selected by kind is applied. */
struct PendingInput final {
    SenseInput sense{};
    IncidentInput incident{};
    ClientStateChangeInput clientStateChange{};
    EntitySlotsRequestedInput entitySlotsRequested{};
    ClientMessageMissionInput clientMessage{};
    ControlRequest control{};
    IncidentRequest incidentControl{};
    ScriptableRequest scriptableControl{};
    PendingKind kind{PendingKind::sense};
};

/** Committed monotonic guards for one full ClientRef identity. */
struct ScriptableGuard final {
    ScriptableTarget target{};
    middleware::bap::activity_message::squad_auth::GenerationGuard squad{};
    middleware::bap::activity_message::scriptable_auth::Type2ChannelState type2{};
    middleware::bap::activity_message::scriptable_auth::Type4GenerationGuard type4{};
    middleware::bap::activity_message::scriptable_auth::Type5RevisionGuard type5{};
    middleware::bap::activity_message::scriptable_auth::Type6GenerationGuard type6{};
    middleware::bap::activity_message::scriptable_auth::Type23SequenceGuard type23{};
    middleware::bap::activity_message::scriptable_auth::Type31GenerationGuard type31{};
    middleware::bap::activity_message::scriptable_auth::Type3GenerationGuard type3{};
    middleware::bap::activity_message::scriptable_auth::Type38GenerationGuard type38{};
    middleware::bap::activity_message::scriptable_auth::Type53SequenceGuard type53{};
    middleware::bap::activity_message::scriptable_auth::Type42GenerationGuard type42{};
    std::uint32_t authoredSceneGeneration{};
    std::uint32_t type2AtomGeneration{};
    bool occupied{};
};

/** One change guard for a decoded type-43 Sense object. */
struct SceneSenseTraceRecord final {
    SenseObservationKey key{};
    std::uint64_t fingerprint{};
    std::uint32_t generationPlusOne{};
    std::uint32_t valueCount{};
    bool hasGeneration{};
    bool occupied{};
};

/** Bounded change guards for one ActivityClient generation. */
struct SceneSenseTrace final {
    std::array<SceneSenseTraceRecord,
               middleware::bap::activity_message::sense_update::kDecodedObjectCapacity>
        records{};
    std::uint64_t sourceGeneration{};
    bool incompleteReported{};
    bool capacityReported{};
};

/** Merged client-reported recovery state for one squad slot. */
struct SquadSenseRecord final {
    SenseObservationKey key{};
    middleware::bap::activity_message::squad_sense::State state{};
};

/** Mutable per-instance storage kept behind the runtime lock. */
struct Instance final {
    InstanceSnapshot view{};
    SenseObservationSnapshot senseObservations{};
    std::vector<SquadSenseRecord> squadSense{};
    std::uint64_t squadSenseSourceGeneration{};
    SceneSenseTrace sceneSenseTrace{};
    std::array<ScriptableGuard, kScriptableGuardCapacity> scriptableGuards{};
    /** Latest delivered body for every full ClientRef, re-emitted by every later msg-5 body. */
    std::vector<PendingScriptableOverride> scriptableAuthEstate{};
    PendingScriptableOverride pendingScriptable{};
    /** Committed bodies waiting on the same push as the head. Never a squad or a lifetime. */
    std::array<PendingScriptableOverride, kPendingScriptableTailCapacity> pendingScriptableTail{};
    std::size_t pendingScriptableTailCount{};
    ScriptableOutputReservation scriptableReservation{};
    std::uint64_t lastTouched{};
    std::uint64_t missionSequence{};
    bool occupied{};
};

/** Clears one instance member by member; a whole-value assignment puts 1 MiB on the stack. */
inline void clear_instance(Instance& instance) noexcept {
    instance.view = {};
    instance.senseObservations = {};
    instance.sceneSenseTrace = {};
    instance.scriptableGuards.fill({});
    std::vector<PendingScriptableOverride>{}.swap(instance.scriptableAuthEstate);
    instance.pendingScriptable = {};
    instance.pendingScriptableTail.fill({});
    instance.pendingScriptableTailCount = 0;
    instance.scriptableReservation = {};
    instance.lastTouched = 0;
    instance.missionSequence = 0;
    instance.occupied = false;
}

extern SRWLOCK g_lock;
extern std::array<Instance, kInstanceCapacity> g_instances;
/** Ordered reducer work grows with real input and fails only when allocation fails. */
extern std::vector<PendingInput> g_pending;
extern std::size_t g_pendingRead;
extern std::size_t g_queuedIngress;
extern std::size_t g_queuedControls;
extern std::uint64_t g_droppedIngress;
extern std::uint64_t g_refusedControls;
extern std::uint64_t g_sequence;
extern std::uint64_t g_eventGeneration;
extern std::uint64_t g_scriptableReservationGeneration;
extern std::uint64_t g_scriptableReservationSequence;

/** Advances a diagnostic counter without making zero look like no event. */
[[nodiscard]] std::uint64_t next_nonzero(std::uint64_t value) noexcept;

/** Finds one exact instance while the runtime lock is held. */
[[nodiscard]] Instance* find_instance(const state::activity::SessionBinding& binding) noexcept;

/** @return True when this exact binding already has an operator request waiting to reduce. */
[[nodiscard]] bool has_queued_control(const state::activity::SessionBinding& binding) noexcept;

/** Appends one owned reducer row while the runtime lock is held. */
[[nodiscard]] bool append_pending(const PendingInput& pending) noexcept;

/** Moves one instance to the newest eviction position. */
void touch(Instance& instance) noexcept;

/** Appends one event in oldest-to-newest ring order. */
void append_event(Event& event) noexcept;

/** Cancels one committed output while the runtime lock is held. */
void cancel_output(Instance& instance, std::uint64_t now) noexcept;

/** Applies one typed request from the shared reducer queue. */
void apply_scriptable_control(const ScriptableRequest& request, std::uint64_t now) noexcept;

// The Sense unit owns these. The reducer and the mission-input feed call into them.

/** Applies one copied msg-6 decode summary. */
void apply_sense(const SenseInput& input, std::uint64_t now) noexcept;

/**
 * Appends one observation and its complete owned value range.
 * @param output Snapshot the observation is appended to; unchanged on failure.
 * @param observation Row to append; its value range is rewritten to the copied span.
 * @param values Values owned by the observation.
 * @return False when the observation or its values do not fit.
 */
[[nodiscard]] bool append_sense_observation(
    SenseObservationSnapshot& output,
    SenseObservation observation,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values) noexcept;

// The incident unit owns these. The reducer, the output cancel and reset call into them.

/** Finds one retained outbound incident revision while the runtime lock is held. */
[[nodiscard]] IncidentRecord* find_incident(const state::activity::SessionBinding& binding,
                                            std::uint64_t revision) noexcept;

/** Copies one incident summary into the chronological event history. */
void fill_incident_event(Event& event,
                         const middleware::bap::activity_message::incident::Incident& incident,
                         std::uint64_t revision) noexcept;

/** Retains one outer-valid client incident for inspection and parsed-field replay. */
void apply_incident(const IncidentInput& input, std::uint64_t now) noexcept;

/** Commits one operator incident into the per-generation ordered output history. */
void apply_incident_control(const IncidentRequest& request, std::uint64_t now) noexcept;

/** Copies the retained incident history and its counters into the diagnostic view. */
void snapshot_incidents(DiagnosticsSnapshot& output) noexcept;

/** Clears every retained incident and its counters. */
void reset_incidents() noexcept;

// The client-message unit owns these. The reducer, the panel snapshot and reset call into them.

/** Publishes one safe generic client envelope into the ordered mission-input feed. */
void apply_client_message(const ClientMessageMissionInput& input, std::uint64_t now) noexcept;

/** Copies the retained framing history and its counter into the diagnostic view. */
void snapshot_client_messages(DiagnosticsSnapshot& output) noexcept;

/** Clears the framing history, the bounded decodes and their counters. */
void reset_client_messages() noexcept;

// The mission-input unit owns these. Every reducer that accepts a client input calls into them.

/** Assigns one exact binding's ordered client mission-input sequence. */
void stamp_mission_sequence(Event& event) noexcept;

/**
 * Retains one accepted client input independently from panel and output events.
 * @param event Accepted input; a row is retained only when it holds a mission sequence.
 * @param sense Complete decode owned by the row, or null.
 * @param clientMessage Generic envelope snapshot owned by the row, or null.
 */
void append_mission_input(
    const Event& event,
    const middleware::bap::activity_message::sense_update::DecodedPacket* sense,
    const ClientMessageSnapshot* clientMessage = nullptr) noexcept;

/** Drops every retained accepted row and restarts the feed sequence. */
void reset_mission_inputs() noexcept;

/** @return The next positive authored-scene generation without changing the guard. */
[[nodiscard]] bool next_authored_scene_generation(std::uint32_t last,
                                                  std::uint32_t& output) noexcept;

/** @return True when one carried group contains the target's exact selected auth slot. */
[[nodiscard]] bool
valid_state_local_group(const ScriptableTarget& target,
                        const state::build_data::scenarios::RosterGroup& group) noexcept;

/** @return True when the bit count and the body agree to within one trailing byte. */
[[nodiscard]] bool valid_auth_storage(std::span<const std::byte> body,
                                      std::size_t bitCount) noexcept;

/** Finds one committed full-slot guard while the runtime lock is held. */
[[nodiscard]] ScriptableGuard* find_guard(Instance& instance,
                                          const ScriptableTarget& target) noexcept;

/** @return True when a transport acknowledgement names the retained body byte-for-byte. */
[[nodiscard]] bool same_pending(const PendingScriptableOverride& left,
                                const PendingScriptableOverride& right) noexcept;

/**
 * Replaces one delivered full-ClientRef body, or appends its first value.
 * @param sourceGeneration Fills the retained row when the body carries no expected generation.
 * @return False when the body does not fit its storage or the estate cannot grow.
 */
[[nodiscard]] bool retain_scriptable_auth(Instance& instance,
                                          const PendingScriptableOverride& pending,
                                          std::uint64_t sourceGeneration) noexcept;

/**
 * Queues one validated scriptable request in the shared ordered control lane.
 * @param reservation Reserved output slot, or null for an ordinary operator request.
 * @return False when the target, the binding or the reservation does not hold.
 */
[[nodiscard]] bool enqueue_request(ScriptableRequest request,
                                   const ScriptableOutputReservation* reservation) noexcept;

/** Clears one exact pending body while the runtime lock is held. */
void cancel_pending(Instance& instance,
                    const state::activity::SessionBinding& binding,
                    std::uint64_t expectedRevision) noexcept;

} // namespace sunrise::server::activity::host::detail
