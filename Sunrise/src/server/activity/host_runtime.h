#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "../../middleware/bap/activity_message/cinematic_incident.h"
#include "../../middleware/bap/activity_message/incident.h"
#include "../../middleware/bap/activity_message/player_trigger_incident.h"
#include "../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../middleware/bap/activity_message/sense_update.h"
#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../middleware/bap/activity_message/squad_auth_body.h"
#include "../../middleware/bap/activity_message/squad_sense_state.h"
#include "../../state/activity/definition.h"
#include "../../state/activity/receipts/definition.h"
#include "../../state/build_data/scenarios/definition.h"
#include "../../state/gameplay/external/squad_entity_retirement.h"

namespace sunrise::server::activity::host {

/** Activity instances tracked by the diagnostic host. */
inline constexpr std::size_t kInstanceCapacity = state::activity::kSessionCapacity;
/** Recent normalized events retained for the panel and HUD. */
inline constexpr std::size_t kEventCapacity = 64;
/**
 * Accepted client mission inputs copied in one paged feed read. The feed itself is unbounded:
 * the reader loops until it is empty, so this only sizes one copy.
 */
inline constexpr std::size_t kMissionInputReadPageSize = 64;
/** Parsed inbound and outbound incidents retained for inspection and field replay. */
inline constexpr std::size_t kIncidentHistoryCapacity = 32;
/** Owned client messages retained as framing metadata for diagnostics. */
inline constexpr std::size_t kClientMessageHistoryCapacity = 512;
/** Decoded packet details retained separately from the high-volume metadata history. */
inline constexpr std::size_t kClientMessageDetailCapacity = 64;
/** Latest complete msg-6 object observations retained for one activity generation. */
inline constexpr std::size_t kSenseObservationCapacity = 128;
/** Typed values owned by the retained msg-6 observations for one activity generation. */
inline constexpr std::size_t kSenseObservationValueCapacity = 1024;
/** Per-slot counts the squad Sense body can carry. Its nested array is eight elements. */
inline constexpr std::size_t kSquadSlotCapacity = 8;
/** Objective task groups a squad publishes one cost for. */
inline constexpr std::size_t kSquadObjectiveGroupCount = 24;
/** Distinct exact ClientRef counters retained for one activity generation. */
inline constexpr std::size_t kScriptableGuardCapacity =
    state::build_data::scenarios::kRosterSlotCapacity;
/** State-local SDK groups are carried with the request instead of indexing the baseline census. */
inline constexpr std::uint16_t kGeneratedRosterGroupIndex = 0xFFFFU;
/** No generated SDK object backs this ordinary canonical roster target. */
inline constexpr std::uint32_t kNoSdkObjectIndex = 0xFFFFFFFFU;
/** Proved spawn-gate-passing type-17 lifetime state used before an operator transition. */
inline constexpr std::uint8_t kDefaultLifetimeState = 3;
/** Lifetime state that shows the loading presentation and refuses the native spawn gate. */
inline constexpr std::uint8_t kLoadingLifetimeState = 4;
/** Highest lifetime state inside the client's jump table. */
inline constexpr std::uint8_t kMaximumLifetimeState = 10;

/** Events visible at the Activity Host boundary. */
enum class EventKind : std::uint8_t {
    /** One bounded msg 6 decode entered the host reducer. */
    senseUpdate = 0,
    /** One committed client msg 22 changed safe numeric State. */
    clientStateChanged = 1,
    /** One owned client envelope without a richer typed mission event entered the host reducer. */
    clientMessageReceived = 2,
    /** An operator state push committed with its pending output. */
    authStateCommitted = 3,
    /** A msg 5 carrying the committed state reached the transport output queue. */
    authStateTransportStaged = 4,
    /** A pending output lost its exact activity generation and was canceled. */
    authStateCanceled = 5,
    /** One validated client msg 19 was captured with all bounded fields. */
    incidentReceived = 6,
    /** One operator msg 19 was accepted into the ordered output history. */
    incidentQueued = 7,
    /** One queued msg 19 reached a matching link's transport output queue. */
    incidentTransportStaged = 8,
    /** One unstaged msg 19 lost its exact activity generation. */
    incidentCanceled = 9,
    /** One queued operator msg 19 was refused during the service slice. */
    incidentRefused = 10,
    /** One typed ClientRef body entered the serialized output slot. */
    scriptableOverrideCommitted = 11,
    /** One typed ClientRef body reached the transport output queue. */
    scriptableOverrideTransportStaged = 12,
    /** One unstaged ClientRef body lost its exact activity generation. */
    scriptableOverrideCanceled = 13,
    /** An operator request was refused without changing state. */
    operatorRefused = 14,
    /** One authoritative Mission State timer became due; this is not a Host feed row. */
    timerElapsed = 15,
    /** One script effect reached a terminal delivery state; this is not a Host feed row. */
    effectResult = 16,
    /** One committed mission phase change; this is not a Host feed row. */
    phaseEntered = 17,
    /** A watched trigger volume became occupied; derived from one msg 6 body. */
    triggerEntered = 18,
    /** A watched trigger volume became empty; derived from one msg 6 body. */
    triggerExited = 19,
    /** One squad's published counters changed; derived from one msg 6 body. */
    squadState = 20,
    /** One squad per-slot count rose; derived from one msg 6 body. */
    entitySpawned = 21,
    /** One squad alive count fell; derived from one msg 6 body. */
    entityDied = 22,
    /** A watched authored scene latched complete; derived from one msg 6 body. */
    sceneFinished = 23,
    /** One watched objective task counter rose; derived from one msg 6 body. */
    objectiveProgress = 24,
    /** Msg 20 reported the exact number of additional simulation-entity indices requested. */
    entitySlotsRequested = 25,
    /** Another committed session bound to this destination; this is not a Host feed row. */
    sessionJoined = 26,
    /** A watched peer session left this destination; this is not a Host feed row. */
    sessionLeft = 27,
    /** One schema-0x8080879F msg-19 target resolved to its authored type-31 source. */
    playerTrigger = 28,
    /** One schema-0x808087BF msg-19 reports that an exact authored Type-6 slot started. */
    cinematicStarted = 29,
    /** One schema-0x808087BF msg-19 reports Type-6 start failure or runtime termination. */
    cinematicTerminated = 30,
    /** Accepted native Ghost interaction progress for one exact type-65 slot. */
    ghostLinkState = 31,
    /** A named actor's movement or delivery level changed. */
    actorPathState = 32,
    /** The client accepted a player's use of one interactable object. */
    objectInteracted = 33,
    /** Native cinematic_skip incident for an exact Type-6 source. */
    cinematicSkipRequested = 34,
    /** Life counts of the private activity's joined party changed. */
    fireteamState = 35,
    /** An interactable object's presence, alive or ownership level changed. */
    objectState = 36,
    /** A damage monitor published new health, shield or revision values. */
    damageState = 37,
};

/** Kinds are numbered without gaps, so the last one plus one is the count. */
inline constexpr std::size_t kEventKindCount =
    static_cast<std::size_t>(EventKind::damageState) + 1U;

/**
 * Terminal delivery outcome of one script-requested effect.
 * Transport staging is the strongest outcome the server can prove. No outcome here says the stock
 * client applied the effect, because the client sends no acknowledgement for one.
 */
enum class EffectOutcome : std::uint8_t {
    /** The carrying message reached the transport output queue. */
    transportStaged,
    /** The host refused the request and committed no state. */
    refused,
    /** The request passed its delivery deadline before it staged. */
    expired,
    /** The request lost its exact activity generation before it staged. */
    canceled,
};

/** Latest attempt to place one committed operator output onto its normal BAP path. */
enum class OutputStatus : std::uint8_t {
    idle,
    pending,
    waitingForEpoch,
    noLayout,
    noGroups,
    noOverrideTarget,
    ambiguousLinks,
    frameRefused,
    transportStaged,
    canceled,
};

/** One serialized operator output slot preserves msg-5/msg-19 order. */
enum class OutputKind : std::uint8_t {
    none,
    authState,
    incident,
    scriptableOverride,
};

/** Full ClientRef slot identity used for counters and transport acknowledgements. */
struct ScriptableTarget final {
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t authSchema{};
    /** Canonical installed roster-table row proved against the whole package object layout. */
    std::uint16_t rosterGroupIndex{};
    /** Canonical slot-array offset proved against this exact slot index and type. */
    std::uint16_t rosterSlotOffset{};
    std::uint16_t slotIndex{};
    /** Generated object row owning an inline state-local group, or the absent sentinel. */
    std::uint32_t sdkObjectIndex{kNoSdkObjectIndex};
    /** Exact full slice-set index for a state-local group, or -1 for a published group. */
    std::int32_t stateLocalRegion{-1};
    std::uint8_t slotType{};
    /** This group is safe only in the exact selected state pinned by the requesting link. */
    bool stateLocalRoster{};
};

/** Exact unarmed Host output lane held while Mission State publishes its matching revision. */
struct ScriptableOutputReservation final {
    state::activity::SessionBinding binding{};
    std::uint64_t resetGeneration{};
    std::uint64_t token{};
    std::uint64_t revision{};
    /** Durable Mission State head, or zero for an operator-only reservation. */
    std::uint64_t intentSequence{};
};

/** Atomic disposition of one exact queued durable Mission output. */
enum class ScriptableWithdrawStatus : std::uint8_t {
    /** The exact queued reducer row was tombstoned before it could commit. */
    withdrawn,
    /** No exact row or output exists and the Host revision is still below the assignment. */
    absent,
    /** The exact Host output is committed and still waiting for transport. */
    committed,
    /** The exact Host output already reached transport. */
    transportStaged,
    /** The exact Host revision committed and was canceled before transport. */
    canceled,
    /** The Host advanced beyond the assigned revision. */
    advanced,
    /** The binding, assignment, or serialized lane did not match exactly. */
    mismatch,
};

/** Typed scriptable body occupying one instance's serialized output slot. */
enum class ScriptableOverrideKind : std::uint8_t {
    squad,
    combatantChannel,
    combatantBinding,
    object,
    sequence,
    cinematic,
    type23,
    type31,
    objectiveReset,
    task,
    authoredScene,
    dialogue,
    sdkAuth,
    /** Activity lifetime state. It carries no body; the msg-5 builder writes the type-17 block. */
    lifetime,
    /** Type-42 performance start: one state name behind a rising per-slot generation. */
    performance,
};

/** Current diagnostic state of one retained incident. */
enum class IncidentStatus : std::uint8_t {
    received,
    queued,
    encodeFailed,
    frameRefused,
    transportStaged,
    canceled,
};

/** What the diagnostic route proved about one owned client body. */
enum class ClientMessageStatus : std::uint8_t {
    unclassified,
    /** The complete declared body was decoded. */
    decoded,
    /** The complete framing closed, but one or more selected values were skipped. */
    decodedPartial,
    /** A typed prefix was decoded and the remaining body is opaque. */
    prefixOnly,
    /** The declared body is bounded, but its inner grammar is not known. */
    opaque,
    /** The msg-19 outer fields decoded, while its target-selected schema remains opaque. */
    outerDecoded,
    /** A transaction accepted the body, but diagnostics kept no parser depth. */
    prepared,
    /** The body parser accepted its input, but State or transaction preparation refused it. */
    prepareRefused,
    malformed,
    quarantined,
};

/** Copied msg-6 fields submitted after envelope and session validation. */
struct SenseInput final {
    state::activity::SessionBinding binding{};
    /** ActivityClient generation that owned the inbound message. */
    std::uint64_t sourceGeneration{};
    /** Protocol ingress row for this same envelope. */
    std::uint64_t clientMessageSequence{};
    std::uint64_t epochFirst{};
    std::uint64_t epochSecond{};
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    std::uint32_t tailBits{};
    std::uint32_t consumedBits{};
    std::uint32_t firstGroupBits{};
    std::uint32_t firstRegistryKey{};
    std::uint32_t groupsSeen{};
    std::uint32_t groupsDecoded{};
    std::uint32_t groupsSkipped{};
    std::uint32_t objectsSeen{};
    std::uint32_t objectsDecoded{};
    std::uint16_t firstSlotIndex{};
    std::uint8_t firstSlotType{};
    middleware::bap::activity_message::sense_update::DecodeStatus decodeStatus{
        middleware::bap::activity_message::sense_update::DecodeStatus::malformed};
    state::activity::receipts::Verdict verdict{state::activity::receipts::Verdict::absent};
    /** Complete value-owned decode copied before decrypted transport storage is cleared. */
    middleware::bap::activity_message::sense_update::DecodedPacket decoded{};
    bool hasFirstObject{};
};

/** Exact package and schema identity for one observed Sense ClientRef. */
struct SenseObservationKey final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint32_t senseSchema{};
    std::uint32_t schemaRow{middleware::bap::activity_message::sense_update::kAbsentRuntimeRow};
    std::uint16_t slotIndex{};
    std::uint8_t slotType{};
};

/** Copies the squad's merged recovery state for this exact ActivityClient generation. */
[[nodiscard]] bool
snapshot_squad_sense(const state::activity::SessionBinding& binding,
                     std::uint64_t sourceGeneration,
                     const SenseObservationKey& key,
                     middleware::bap::activity_message::squad_sense::State& output) noexcept;

/** One latest complete msg-6 object observation with values in its owning snapshot. */
struct SenseObservation final {
    state::activity::SessionBinding binding{};
    SenseObservationKey key{};
    std::uint64_t sequence{};
    std::uint64_t tick{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t generationPlusOne{};
    std::uint32_t firstValue{};
    std::uint32_t valueCount{};
    bool hasGeneration{};
};

/** Value-owned latest observations for one exact activity and ActivityClient generation. */
struct SenseObservationSnapshot final {
    std::array<SenseObservation, kSenseObservationCapacity> observations{};
    std::array<middleware::bap::activity_message::sense_update::DecodedValue,
               kSenseObservationValueCapacity>
        values{};
    std::size_t observationCount{};
    std::size_t valueCount{};
    std::uint64_t revision{};
    std::uint64_t sourceGeneration{};
};

/** Exact reflected scalar identity inside one retained Sense ClientRef. */
struct SenseScalarIdentity final {
    SenseObservationKey object{};
    std::uint32_t fieldSchemaRow{
        middleware::bap::activity_message::sense_update::kAbsentRuntimeRow};
    std::uint32_t fieldRow{middleware::bap::activity_message::sense_update::kAbsentRuntimeRow};
    std::uint32_t occurrence{};
    std::uint16_t fieldOrdinal{};
    middleware::bap::activity_message::sense_update::ValueKind kind{
        middleware::bap::activity_message::sense_update::ValueKind::unsignedInteger};
};

/** Value-owned scalar copied from one exact retained Sense observation. */
struct SenseScalarSample final {
    state::activity::SessionBinding binding{};
    SenseScalarIdentity identity{};
    middleware::bap::activity_message::sense_update::DecodedValue value{};
    std::uint64_t observationRevision{};
    std::uint64_t tick{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t generationPlusOne{};
    bool hasGeneration{};
};

/** Fail-closed result from selecting one exact scalar from a value-owned Sense snapshot. */
enum class SenseScalarStatus : std::uint8_t {
    ready,
    invalidIdentity,
    invalidSnapshot,
    notFound,
    ambiguous,
};

/** Outer-valid msg-19 fields copied after envelope and exact-session ownership checks. */
struct IncidentInput final {
    state::activity::SessionBinding binding{};
    middleware::bap::activity_message::incident::Incident incident{};
    std::uint64_t sourceGeneration{};
    /** Protocol ingress row for this same envelope. */
    std::uint64_t clientMessageSequence{};
    std::uint32_t payloadBytes{};
    middleware::bap::activity_message::player_trigger_incident::Payload playerTrigger{};
    middleware::bap::activity_message::cinematic_incident::Payload cinematic{};
    middleware::bap::activity_message::cinematic_incident::Signal cinematicSignal{
        middleware::bap::activity_message::cinematic_incident::Signal::started};
    bool hasPlayerTrigger{};
    bool hasCinematic{};
};

/** Safe msg-22 after-image submitted only after State and connection publication commit. */
struct ClientStateChangeInput final {
    state::activity::SessionBinding binding{};
    state::activity::membership::CommittedClientState state{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t payloadBytes{};
};

/** A committed msg-20 request emitted by the client's simulation-entity low-water path. */
struct EntitySlotsRequestedInput final {
    state::activity::SessionBinding binding{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::int32_t requestedCount{};
};

/** Framing metadata copied after exact ActivityClient ownership checks. */
struct ClientMessageInput final {
    state::activity::SessionBinding binding{};
    state::activity::membership::AuthoritativeUpdate authoritative{};
    std::uint64_t sourceGeneration{};
    std::uint64_t payloadFingerprint{};
    std::uint32_t messageType{};
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    std::uint32_t consumedBits{};
    ClientMessageStatus status{ClientMessageStatus::unclassified};
    bool hasPayloadFingerprint{};
    bool hasAuthoritative{};
};

/** One value-owned event, with no view into decrypted transport storage. */
struct Event final {
    state::activity::SessionBinding binding{};
    /** Present only for an internally synthesized timerElapsed callback. */
    state::activity::mission::StateKey timerName{};
    std::uint64_t sequence{};
    std::uint64_t tick{};
    union {
        std::uint64_t stateRevision{};
        /** Scriptable revision for scriptable output events. */
        std::uint64_t scriptableRevision;
    };
    std::uint64_t sourceGeneration{};
    /** Absolute service tick committed when the timer was armed. */
    std::uint64_t timerDeadlineTick{};
    /** Stable timer identity allocated by authoritative Mission State. */
    std::uint64_t
        timerSequence{}; /** Durable intent sequence the effect call returned to the script. */
    std::uint64_t effectRequestKey{};
    /** Peer session id, for sessionJoined and sessionLeft events. */
    std::uint64_t peerSessionId{};
    /** Peer record generation, so a replacement is told apart from the record it replaced. */
    std::uint64_t peerSessionGeneration{};
    /** Peer client key bound by its join, or zero before one. */
    std::uint64_t peerMemberKey{};
    /** Party life counts, for fireteamState events. */
    std::uint16_t fireteamAlive{};
    std::uint16_t fireteamDead{};
    std::uint16_t fireteamUnknown{};
    /** Committed mission phase, for phaseEntered events. */
    std::uint32_t missionPhase{};
    /** Mission phase this commit replaced. */
    std::uint32_t previousMissionPhase{};
    /** Owning object of the slot a derived Sense edge came from. */
    std::uint32_t slotObjectTag{};
    /** Root Sense schema of the slot a derived Sense edge came from. */
    std::uint32_t slotSenseSchema{};
    /** Members of the watched set inside the volume. */
    std::int32_t triggerCount{};
    /** Member count the server asked the client to compare against. */
    std::int32_t triggerValue{};
    /** True when the whole watched set is inside the volume. */
    bool triggerAll{};
    /** Health and shield fractions, for damageState events. Negative until published. */
    float damageHealth{-1.0F};
    float damageShield{-1.0F};
    std::int32_t damageRevision{};
    /** Object level, for objectState and objectInteracted events. */
    std::int32_t objectGeneration{};
    bool objectPresent{};
    bool objectAlive{};
    bool objectOwnerKnown{};
    bool objectHasOwner{};
    std::uint64_t objectOwnerKey{};
    /** Ghost-link level, for ghostLinkState events. */
    std::int32_t ghostGeneration{};
    float ghostProgress{};
    bool ghostActive{};
    /** Named actor level, for actorPathState events. */
    std::int32_t actorGeneration{};
    std::int32_t actorPathRevision{};
    std::int32_t actorPathState{};
    std::int32_t actorDeliveryRevision{};
    std::int32_t actorDeliveryState{};
    bool actorDeliveryKnown{};
    bool actorDead{};
    /** Objective costs the squad published, one per task group, with a bit per known cost. */
    std::array<float, kSquadObjectiveGroupCount> squadObjectiveCosts{};
    std::uint32_t squadObjectiveCostMask{};
    std::uint32_t squadObjectiveRevision{};
    /** Per-slot member counts the client published, for squadState events. */
    std::array<std::int32_t, kSquadSlotCapacity> squadSlotCounts{};
    /** Alive members the client published. Six bits on the wire, so 0 through 63. */
    std::int32_t squadAliveCount{};
    /** Alive count this observation replaced, for entityDied events. */
    std::int32_t squadPreviousAliveCount{};
    /** New count of the one slot that rose, for entitySpawned events. */
    std::int32_t squadSlotValue{};
    /** Count that slot held before it rose. */
    std::int32_t squadPreviousSlotValue{};
    /** Live entries in squadSlotCounts, from the list's four-bit length. */
    std::uint8_t squadSlotCountLength{};
    /** Index into squadSlotCounts that rose, for entitySpawned events. */
    std::uint8_t squadSlotOrdinal{};
    /** Client flag written on its actor death or removal path. The meaning is unproved. */
    bool squadRemovalFlag{};
    /** Activation token the completed authored scene echoed, for sceneFinished events. */
    std::int32_t sceneActivationToken{};
    /** Task counter the client published, for objectiveProgress events. Clamped to 80. */
    std::int32_t objectiveTaskCount{};
    /** Counter that task held before it rose. */
    std::int32_t objectivePreviousTaskCount{};
    /** Objective this task belongs to. The block is 24 long. */
    std::uint8_t objectiveOrdinal{};
    /** Task inside that objective. The list is 24 long. */
    std::uint8_t objectiveTaskOrdinal{};
    std::uint64_t clientMessageSequence{};
    std::uint64_t epochFirst{};
    std::uint64_t epochSecond{};
    union {
        std::uint64_t incidentRevision{};
        /** Per-binding order for client Sense/incident inputs. */
        std::uint64_t missionSequence;
    };
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    std::uint32_t tailBits{};
    std::uint32_t consumedBits{};
    std::uint32_t firstGroupBits{};
    union {
        /** First decoded registry key for Sense events. */
        std::uint32_t firstRegistryKey{};
        /** Committed region index for client-state events. */
        std::int32_t regionIndex;
    };
    std::uint32_t groupsSeen{};
    std::uint32_t groupsDecoded{};
    std::uint32_t groupsSkipped{};
    std::uint32_t objectsSeen{};
    std::uint32_t objectsDecoded{};
    union {
        /** Primary target for incident events. */
        std::uint32_t incidentTarget{};
        /** Associated D6 slice-set hash for client-state events. */
        std::uint32_t regionSliceSetHash;
    };
    /** Activity State revision from the committed msg-22 after-image. */
    std::uint64_t activityStateRevision{};
    /** The region the client holds, for client-state events whose current leg moved. */
    std::int32_t currentRegionIndex{-1};
    /**
     * The region the client holds after any client-state event, moved or not.
     * A report that moves only the pending leg still leaves the client standing where it was, and
     * the leg it did not restate reads absent. -1 while the client holds no region.
     */
    std::int32_t heldRegionIndex{state::activity::membership::kAbsentRegionIndex};
    /** Membership revision from the committed msg-22 after-image. */
    std::uint32_t membershipRevision{};
    std::uint32_t teleportSliceSetHash{};
    std::int32_t teleportSliceSetIndex{state::activity::membership::kAbsentSliceSetIndex};
    std::int8_t spawnState{};
    std::int8_t teleportState{};
    std::uint32_t incidentExtraTargets{};
    std::uint32_t incidentSelectorBytes{};
    std::uint32_t incidentPayloadBytes{};
    /** Type-31 source ClientRef carried by a schema-0x8080879F player-trigger incident. */
    std::uint32_t playerTriggerRegistryKey{};
    std::uint32_t playerTriggerResolvedObjectId{};
    std::int16_t playerTriggerSlotIndex{-1};
    std::int8_t playerTriggerSlotType{-1};
    /** Type-6 source ClientRef carried by a schema-0x808087BF cinematic incident. */
    std::uint32_t cinematicRegistryKey{};
    std::uint64_t cinematicRuntimeObjectId{};
    float cinematicEventValue{};
    std::int16_t cinematicSlotIndex{-1};
    std::int8_t cinematicSlotType{-1};
    middleware::bap::activity_message::cinematic_incident::Signal cinematicSignal{
        middleware::bap::activity_message::cinematic_incident::Signal::started};
    /** Client activity-message id for clientMessageReceived events. */
    std::uint32_t clientMessageType{};
    /** Exact positive count decoded from an entitySlotsRequested msg-20 event. */
    std::int32_t requestedEntitySlots{};
    /** Safe retained scalar count for clientMessageReceived events. */
    std::uint16_t firstSlotIndex{};
    std::uint8_t firstSlotType{};
    std::uint8_t lifetimeState{kDefaultLifetimeState};
    /** Mission action kind of the requested effect, for effectResult events. */
    std::uint8_t effectAction{};
    EffectOutcome effectOutcome{EffectOutcome::transportStaged};
    EventKind kind{EventKind::senseUpdate};
    ClientMessageStatus clientMessageStatus{ClientMessageStatus::unclassified};
    middleware::bap::activity_message::sense_update::DecodeStatus senseDecodeStatus{
        middleware::bap::activity_message::sense_update::DecodeStatus::malformed};
    state::activity::receipts::Verdict verdict{state::activity::receipts::Verdict::absent};
    /** The packet's observations were retained, so a Lua snapshot may be built from them. */
    bool senseSnapshotRetained{};
    bool hasFirstObject{};
    bool clientStateHasRegion{};
    bool clientStateHasCurrentRegion{};
    bool clientStateHasSpawn{};
    bool clientStateHasTeleport{};
    bool hasPlayerTrigger{};
    bool hasCinematic{};

    /** @return True when this Sense event carries observations a script may read. */
    [[nodiscard]] bool has_sense_observations() const noexcept {
        using Status = middleware::bap::activity_message::sense_update::DecodeStatus;
        return kind == EventKind::senseUpdate && senseSnapshotRetained
               && (senseDecodeStatus == Status::complete || senseDecodeStatus == Status::partial);
    }
};

/** Position after one event in one reset generation. */
struct EventCursor final {
    /** Reset generation owning sequence. */
    std::uint64_t generation{};
    std::uint64_t sequence{};
};

/** Value-owned result from one bounded event feed read. */
struct EventRead final {
    std::array<Event, kEventCapacity> events{};
    EventCursor cursor{};
    std::size_t count{};
    /** Known events overwritten between the input cursor and the first copied row. */
    std::uint64_t missed{};
    /** At least one event in the current generation was overwritten before this read. */
    bool gap{};
    /** The input cursor named another reset generation. */
    bool reset{};
};

/** One accepted client mission input with its independent feed position. */
struct MissionInputEvent final {
    Event event{};
    std::uint64_t sequence{};
};

/** Position after one accepted client mission input in one reset generation. */
struct MissionInputCursor final {
    std::uint64_t generation{};
    std::uint64_t sequence{};
};

/** Value-owned accepted mission inputs after one cursor. */
struct MissionInputRead final {
    std::array<MissionInputEvent, kMissionInputReadPageSize> events{};
    MissionInputCursor cursor{};
    std::size_t count{};
    /** Accepted rows between the cursor and the first retained row. */
    std::uint64_t missed{};
    /** The cursor is behind the oldest retained row. A trigger, not proof of loss. */
    bool gap{};
    bool reset{};
};

/** Current compact state and output status for one exact activity generation. */
struct InstanceSnapshot final {
    state::activity::SessionBinding binding{};
    std::uint64_t stateRevision{};
    std::uint64_t transportRevision{};
    std::uint64_t lastEventSequence{};
    std::uint64_t lastOutputAttemptTick{};
    std::uint64_t lastOutputSourceGeneration{};
    std::uint64_t incidentRevision{};
    std::uint64_t incidentTransportRevision{};
    std::uint64_t scriptableRevision{};
    std::uint64_t scriptableTransportRevision{};
    std::uint64_t scriptableReservedRevision{};
    std::uint32_t senseCount{};
    std::uint32_t outputAttempts{};
    std::uint32_t incidentsReceived{};
    std::uint32_t incidentsQueued{};
    std::uint32_t incidentsPending{};
    std::uint32_t senseObservationCount{};
    std::uint32_t senseObservationValueCount{};
    std::uint64_t senseObservationRevision{};
    std::uint64_t senseObservationSourceGeneration{};
    std::uint8_t lifetimeState{kDefaultLifetimeState};
    OutputStatus outputStatus{OutputStatus::idle};
    OutputKind outputKind{OutputKind::none};
    bool outputPending{};
    bool scriptableReservationPending{};
    bool active{};
};

/** Full retained incident body and its diagnostic transport state. */
struct IncidentRecord final {
    state::activity::SessionBinding binding{};
    middleware::bap::activity_message::incident::Incident incident{};
    std::uint64_t sequence{};
    std::uint64_t tick{};
    std::uint64_t revision{};
    std::uint64_t lastAttemptTick{};
    std::uint64_t lastSourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t payloadBytes{};
    std::uint32_t attempts{};
    std::uint32_t transportStages{};
    IncidentStatus status{IncidentStatus::received};
    bool outbound{};
};

/** Compact exact generation key used by high-volume diagnostic rows. */
struct ClientMessageBinding final {
    std::uint64_t sessionId{};
    std::uint64_t createdRevision{};
};

/** One owned client message, retained without its sensitive payload. */
struct ClientMessageRecord final {
    ClientMessageBinding binding{};
    state::activity::membership::AuthoritativeUpdate authoritative{};
    std::uint64_t sequence{};
    std::uint64_t tick{};
    std::uint64_t sourceGeneration{};
    std::uint64_t payloadFingerprint{};
    std::uint32_t messageType{};
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    std::uint32_t consumedBits{};
    ClientMessageStatus status{ClientMessageStatus::unclassified};
    bool hasPayloadFingerprint{};
    bool hasAuthoritative{};
};

/** One typed Sense decode retained for a selected ingress row. */
struct ClientMessageDetail final {
    middleware::bap::activity_message::sense_update::DecodedPacket sense{};
    std::uint64_t sequence{};
    std::uint32_t messageType{};
    bool hasSenseDecode{};
};

/** Value-owned protocol-message identity attached to one accepted mission-input row. */
struct ClientMessageSnapshot final {
    std::uint32_t messageType{};
    ClientMessageStatus status{ClientMessageStatus::unclassified};
};

/** Complete immutable view copied to one UI frame. */
struct DiagnosticsSnapshot final {
    std::array<InstanceSnapshot, kInstanceCapacity> instances{};
    std::array<Event, kEventCapacity> events{};
    std::array<IncidentRecord, kIncidentHistoryCapacity> incidents{};
    std::array<ClientMessageRecord, kClientMessageHistoryCapacity> clientMessages{};
    std::size_t instanceCount{};
    std::size_t eventCount{};
    std::size_t incidentCount{};
    std::size_t clientMessageCount{};
    std::uint64_t droppedIngress{};
    std::uint64_t droppedIncidents{};
    std::uint64_t queuedControls{};
    std::uint64_t refusedControls{};
    std::uint64_t refusedIncidents{};
    std::uint64_t overwrittenEvents{};
    std::uint64_t overwrittenIncidents{};
    std::uint64_t overwrittenClientMessages{};
};

/** Activity-state value consumed by the msg-5 output adapter. */
struct AuthState final {
    std::uint64_t revision{};
    std::uint8_t lifetimeState{kDefaultLifetimeState};
};

/** Immutable outbound incident selected after one connection's staged revision. */
struct PendingIncident final {
    middleware::bap::activity_message::incident::Incident incident{};
    std::uint64_t revision{};
};

/** Immutable typed body retained byte-for-byte until exact transport staging. */
struct PendingScriptableOverride final {
    state::gameplay::squad_entity_retirement::Eligibility squadRetirement{};
    std::array<std::byte,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideByteCapacity>
        body{};
    ScriptableTarget target{};
    /** Exact generated group retained for a generation-bound state-local SDK request. */
    state::build_data::scenarios::RosterGroup stateLocalRosterGroup{};
    std::uint64_t revision{};
    std::uint64_t generation{};
    /** ActivityClient generation that authorized this request, or zero for unpinned input. */
    std::uint64_t expectedActivityClientGeneration{};
    std::int16_t sequence{};
    std::int32_t dialogueSequence{};
    std::uint32_t channelHash{};
    float channelValue{};
    std::uint16_t bitCount{};
    std::uint16_t byteCount{};
    std::uint16_t dialogueCue{};
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel{};
    ScriptableOverrideKind kind{ScriptableOverrideKind::type23};
    /** Activity lifetime state for a lifetime request; ignored by every other kind. */
    std::uint8_t lifetimeState{kDefaultLifetimeState};
    bool sdkCompiled{};
};

/**
 * A burst of mission actions commits together. The head keeps every existing push behaviour and
 * the tail rides out with it as ordinary retained rows, so one push carries the whole burst.
 */
inline constexpr std::size_t kPendingScriptableTailCapacity = 63;

/**
 * Copies the committed bodies waiting on the same push as the head, oldest first.
 * @return Count written, never more than the output span.
 */
[[nodiscard]] std::size_t
pending_scriptable_tail(const state::activity::SessionBinding& binding,
                        std::span<PendingScriptableOverride> output) noexcept;

/** Copies the latest delivered Auth body for every full ClientRef in this client generation. */
[[nodiscard]] bool scriptable_auth_estate(const state::activity::SessionBinding& binding,
                                          std::uint64_t activityClientGeneration,
                                          std::vector<PendingScriptableOverride>& output) noexcept;

/** Queues one owned msg-6 prefix for the Activity Host service. */
[[nodiscard]] bool submit_sense(const SenseInput& input) noexcept;

/** Queues one owned, outer-valid client msg 19 for the Activity Host service. */
[[nodiscard]] bool submit_incident(const IncidentInput& input) noexcept;

/** Queues one committed client msg-22 numeric after-image for the Activity Host service. */
[[nodiscard]] bool submit_client_state_change(const ClientStateChangeInput& input) noexcept;

/** Queues one committed msg-20 simulation-entity slot request for the Activity Host service. */
[[nodiscard]] bool submit_entity_slots_requested(const EntitySlotsRequestedInput& input) noexcept;

/** Retains one owned client message without entering the reducer queue. */
[[nodiscard]] std::uint64_t record_client_message(
    const ClientMessageInput& input,
    const middleware::bap::activity_message::sense_update::DecodedPacket* sense = nullptr) noexcept;

/** Queues one owned client envelope that has no richer typed mission-input reducer. */
[[nodiscard]] bool submit_client_message(const ClientMessageInput& input,
                                         std::uint64_t clientMessageSequence) noexcept;

/** Copies one retained scalar decode by its ingress sequence. */
[[nodiscard]] bool client_message_detail(std::uint64_t sequence,
                                         ClientMessageDetail& output) noexcept;

/** Queues one operator Auth-state transition for the exact activity generation. */
[[nodiscard]] bool request_auth_state(const state::activity::SessionBinding& binding,
                                      std::uint8_t lifetimeState) noexcept;

/** Queues one outer-valid operator msg 19 for the exact activity generation. */
[[nodiscard]] bool
request_incident(const state::activity::SessionBinding& binding,
                 const middleware::bap::activity_message::incident::Incident& incident) noexcept;

/** Queues one generation-bound type-23 update for an exact package-derived ClientRef. */
[[nodiscard]] bool
request_type23_override(const state::activity::SessionBinding& binding,
                        const ScriptableTarget& target,
                        middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
                        float value,
                        bool snap,
                        std::uint64_t expectedActivityClientGeneration,
                        const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored object entry activation from an exact generated roster group. */
[[nodiscard]] bool request_state_local_type4_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t entryIndex,
    bool active,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr,
    const ScriptableOutputReservation* burstHead = nullptr) noexcept;

/** Queues one named actor behavior-channel write from an exact generated type-2 slot. */
[[nodiscard]] bool request_state_local_type2_channel_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint32_t channelHash,
    float value,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues squad-member binding for one exact generated type-2 combatant slot. */
[[nodiscard]] bool request_state_local_type2_squad_binding(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues a generation-bound type-23 update from an exact generated roster group. */
[[nodiscard]] bool request_state_local_type23_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
    float value,
    bool snap,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one type-31 pulse for an exact package-derived ClientRef. */
[[nodiscard]] bool
request_type31_override(const state::activity::SessionBinding& binding,
                        const ScriptableTarget& target,
                        const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one type-31 pulse carried by the exact generated group in the current activity seed. */
[[nodiscard]] bool request_state_local_type31_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored sequence restart from the exact generated mission group. */
[[nodiscard]] bool request_state_local_sequence_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored cinematic start or stop from the exact generated mission group. */
[[nodiscard]] bool request_state_local_cinematic_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    bool active,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one type-42 performance start naming a state of the sensor's target actor. */
[[nodiscard]] bool request_state_local_performance_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint32_t stateNameHash,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one full objective reset from the exact generated group in the mission seed. */
[[nodiscard]] bool request_state_local_objective_reset(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored-task generation from the exact generated group in the mission seed. */
[[nodiscard]] bool request_state_local_task_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored-scene activation from the exact generated group in the mission seed. */
[[nodiscard]] bool request_state_local_authored_scene_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored dialogue line from the exact generated group in the mission seed. */
[[nodiscard]] bool request_state_local_dialogue_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint16_t cueIndex,
    std::uint16_t authoredCueCount,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one squad placement intent for an exact package-derived ClientRef. */
[[nodiscard]] bool request_squad_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
    std::span<const std::int32_t> requestedCounts,
    middleware::bap::activity_message::squad_auth::Mode mode,
    std::uint64_t expectedActivityClientGeneration,
    std::optional<std::uint32_t> nameHash = std::nullopt,
    const ScriptableOutputReservation* reservation = nullptr,
    std::array<std::int8_t, 4> authoredProfile = {},
    state::gameplay::squad_entity_retirement::Eligibility squadRetirement = {}) noexcept;

/** Queues one generation-bound activity lifetime state through the serialized output slot. */
[[nodiscard]] bool
request_lifetime_override(const state::activity::SessionBinding& binding,
                          std::uint8_t lifetimeState,
                          std::uint64_t expectedActivityClientGeneration,
                          const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one exact SDK-compiled Auth body for a generation-bound ClientRef. */
[[nodiscard]] bool
request_sdk_auth_override(const state::activity::SessionBinding& binding,
                          const ScriptableTarget& target,
                          const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
                          std::span<const std::byte> body,
                          std::uint16_t bitCount,
                          std::uint64_t expectedActivityClientGeneration,
                          const ScriptableOutputReservation* reservation = nullptr) noexcept;

/** @return True while any live instance holds a committed output the transport has not sent. */
[[nodiscard]] bool any_output_pending() noexcept;

/** Holds one exact next scriptable revision without queueing or exposing an output body. */
[[nodiscard]] bool reserve_scriptable_output(const state::activity::SessionBinding& binding,
                                             ScriptableOutputReservation& output,
                                             std::uint64_t intentSequence = 0) noexcept;

/** Releases only the exact unarmed reservation returned by reserve_scriptable_output. */
[[nodiscard]] bool
release_scriptable_output(const ScriptableOutputReservation& reservation) noexcept;

/**
 * Atomically withdraws an exact queued Mission reducer row or reports its committed disposition.
 * A caller may release durable State ownership only after `withdrawn`, `absent`, or `canceled`.
 */
[[nodiscard]] ScriptableWithdrawStatus
withdraw_scriptable_output(const state::activity::SessionBinding& binding,
                           std::uint64_t intentSequence,
                           std::uint64_t expectedRevision) noexcept;

/** Applies queued client and operator events on the server service slice. */
void service(std::uint64_t now) noexcept;

/** Copies the latest complete diagnostic view. */
void snapshot(DiagnosticsSnapshot& output) noexcept;
/** Copies one exact activity generation without exposing the Host lock. */
[[nodiscard]] bool instance_snapshot(const state::activity::SessionBinding& binding,
                                     InstanceSnapshot& output) noexcept;

/** Reads the current feed position without replaying retained history. */
[[nodiscard]] EventCursor current_event_cursor() noexcept;

/**
 * Copies retained events after one cursor and advances output.cursor to the newest row.
 * A zero cursor returns retained rows and reports reset against the live generation.
 */
void read_events_after(EventCursor after, EventRead& output) noexcept;

/** Reads the current accepted-mission-input position without replaying retained history. */
[[nodiscard]] MissionInputCursor current_mission_input_cursor() noexcept;

/**
 * Copies accepted client mission inputs after one cursor.
 * A row is dropped only once durable mission State no longer owes it. A read is not a commit.
 */
void read_mission_inputs_after(MissionInputCursor after, MissionInputRead& output) noexcept;

/** Copies the exact Sense values owned by one retained accepted-input row. */
[[nodiscard]] bool mission_input_sense_snapshot(std::uint64_t sequence,
                                                SenseObservationSnapshot& output) noexcept;

/** Copies the safe generic values owned by one retained accepted-input row. */
[[nodiscard]] bool mission_input_client_message_snapshot(std::uint64_t sequence,
                                                         ClientMessageSnapshot& output) noexcept;

/** Copies the latest complete Sense observations for one exact activity generation. */
[[nodiscard]] bool snapshot_sense_observations(const state::activity::SessionBinding& binding,
                                               SenseObservationSnapshot& output) noexcept;

/** Selects one exact reflected scalar without changing the retained snapshot. */
[[nodiscard]] SenseScalarStatus select_sense_scalar(const SenseObservationSnapshot& snapshot,
                                                    const state::activity::SessionBinding& binding,
                                                    const SenseScalarIdentity& identity,
                                                    SenseScalarSample& output) noexcept;

/** @return True when two selected scalars have the same presence and raw typed value. */
[[nodiscard]] bool same_sense_scalar_value(const SenseScalarSample& left,
                                           const SenseScalarSample& right) noexcept;

/** @return True when both samples name the same ActivityClient and reported object generations. */
[[nodiscard]] bool same_sense_scalar_generations(const SenseScalarSample& left,
                                                 const SenseScalarSample& right) noexcept;

/** @return Stable text for one exact scalar-selection result. */
[[nodiscard]] const char* sense_scalar_status_name(SenseScalarStatus status) noexcept;

/** Reads the committed Auth state for one exact activity generation. */
[[nodiscard]] bool auth_state(const state::activity::SessionBinding& binding,
                              AuthState& output) noexcept;

/** Reads the one pending incident occupying the exact instance's serialized output slot. */
[[nodiscard]] bool pending_incident(const state::activity::SessionBinding& binding,
                                    PendingIncident& output) noexcept;

/** Reads the one pending typed ClientRef body without changing its counter. */
[[nodiscard]] bool pending_scriptable_override(const state::activity::SessionBinding& binding,
                                               PendingScriptableOverride& output) noexcept;

/**
 * Reads an override for one exact ActivityClient generation.
 * A state-local request authorized by another generation is atomically canceled here.
 */
[[nodiscard]] bool
pending_scriptable_override_for_activity_client(const state::activity::SessionBinding& binding,
                                                std::uint64_t activityClientGeneration,
                                                PendingScriptableOverride& output) noexcept;

/** Cancels the exact instance's raw incident before any later transport staging. */
[[nodiscard]] bool cancel_pending_incident(const state::activity::SessionBinding& binding) noexcept;

/** Cancels one exact unstaged typed override revision without advancing its slot counter. */
[[nodiscard]] bool
cancel_pending_scriptable_override(const state::activity::SessionBinding& binding,
                                   std::uint64_t expectedRevision) noexcept;

/** Records one refused BAP attempt without clearing the committed output. */
void note_auth_attempt(const state::activity::SessionBinding& binding,
                       std::uint64_t sourceGeneration,
                       std::uint64_t revision,
                       std::uint8_t lifetimeState,
                       OutputStatus status) noexcept;

/** Marks one committed Auth revision as staged into the transport output queue. */
void note_auth_transport_staged(const state::activity::SessionBinding& binding,
                                std::uint64_t sourceGeneration,
                                std::uint64_t revision,
                                std::uint8_t lifetimeState) noexcept;

/** Records one failed attempt without advancing a connection's incident cursor. */
void note_incident_attempt(const state::activity::SessionBinding& binding,
                           std::uint64_t sourceGeneration,
                           std::uint64_t revision,
                           IncidentStatus status) noexcept;

/** Marks one retained incident revision as staged into a matching transport output queue. */
void note_incident_transport_staged(const state::activity::SessionBinding& binding,
                                    std::uint64_t sourceGeneration,
                                    std::uint64_t revision) noexcept;

/** Records one refused typed-body attempt without consuming its sequence or generation. */
void note_scriptable_attempt(const state::activity::SessionBinding& binding,
                             std::uint64_t sourceGeneration,
                             const PendingScriptableOverride& pending,
                             OutputStatus status) noexcept;

/** Commits one exact typed body and only then advances its full-slot counter. */
void note_scriptable_transport_staged(const state::activity::SessionBinding& binding,
                                      std::uint64_t sourceGeneration,
                                      const PendingScriptableOverride& pending) noexcept;

/** Clears every instance, queue, and diagnostic counter. */
void reset() noexcept;

/** @return Stable UI name for one event kind. */
[[nodiscard]] const char* event_name(EventKind kind) noexcept;

/** @return Stable UI name for one framing verdict. */
[[nodiscard]] const char* verdict_name(state::activity::receipts::Verdict verdict) noexcept;

/** @return Stable UI name for one output attempt status. */
[[nodiscard]] const char* output_status_name(OutputStatus status) noexcept;

/** @return Stable UI name for one retained incident status. */
[[nodiscard]] const char* incident_status_name(IncidentStatus status) noexcept;

/** @return Stable diagnostic name for one client message type. */
[[nodiscard]] const char* client_message_name(std::uint32_t messageType) noexcept;

/** @return Stable diagnostic name for one client body status. */
[[nodiscard]] const char* client_message_status_name(ClientMessageStatus status) noexcept;

} // namespace sunrise::server::activity::host
