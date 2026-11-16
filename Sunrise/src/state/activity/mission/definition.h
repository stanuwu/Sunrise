#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../../core/network_capacity.h"

namespace sunrise::state::activity::mission {

/** One activity generation retains at most 512 script variables. */
inline constexpr std::size_t kVariableCapacity = 512;
/** One activity generation retains at most thirty-two authoritative timers. */
inline constexpr std::size_t kTimerCapacity = 32;
/** Durable variable and timer names contain at most sixty-three bytes. */
inline constexpr std::size_t kStateKeyByteCapacity = 64;
/** Durable script strings contain at most one hundred twenty-seven bytes. */
inline constexpr std::size_t kVariableStringByteCapacity = 128;
/** One action may name a run of object slots that answer on a single reserved revision. */
inline constexpr std::size_t kIntentBurstCapacity = 63;
/** A mission leaves at most 32 objects out of the seeds it selects; the pike sets need 16. */
inline constexpr std::size_t kMissionSeedOmitCapacity = 32;
/** Squad Auth accepts at most fifteen authored member counts. */
inline constexpr std::size_t kSquadMemberCapacity = 15;
/** Squad Auth `.2` holds at most six temperaments. */
inline constexpr std::size_t kSquadTemperamentCapacity = 6;
/** A compiled Slot Auth body must fit the widest bounded msg-5 table. */
inline constexpr std::size_t kMaximumAuthBodyByteCount = (53'150U + 7U) / 8U;
/** Zero is reserved as the absent durable intent sequence. */
inline constexpr std::uint64_t kAbsentIntentSequence = 0;
/** Durable intent sequences begin at one. */
inline constexpr std::uint64_t kFirstIntentSequence = 1;
/** Zero is reserved as the absent script request key. */
inline constexpr std::uint64_t kAbsentIntentKey = 0;
/** Script request keys begin at one. */
inline constexpr std::uint64_t kFirstIntentKey = 1;
/** Zero means that no Host output revision owns the intent yet. */
inline constexpr std::uint64_t kAbsentHostOutputRevision = 0;
/** Zero is reserved as the absent durable timer sequence. */
inline constexpr std::uint64_t kAbsentTimerSequence = 0;
/** Durable timer sequences begin at one. */
inline constexpr std::uint64_t kFirstTimerSequence = 1;
/** A new bound activity begins with its first mission attempt. */
inline constexpr std::uint64_t kFirstAttemptGeneration = 1;
/** Type-17 state six completes the mission without inventing a timed state ladder. */
inline constexpr std::uint8_t kCompletedLifetimeState = 6;

/** Only accepted native effects change the mission attempt owner. */
struct AttemptState final {
    std::uint64_t generation{kFirstAttemptGeneration};
    bool complete{};
};

/** Squad Sense publishes a six-bit alive count. */
inline constexpr std::int32_t kMaximumSquadAlive = 63;

/**
 * Count evidence belongs to one transported placement and its echoed spawn generation, as the
 * client of the link that carried the placement reports it.
 */
struct SquadPopulation final {
    std::uint64_t requestKey{};
    std::uint64_t attemptGeneration{};
    /** Link whose client simulates the squad: the one it was staged on, until a re-pin moves it. */
    std::uint64_t sourceGeneration{};
    std::uint64_t inputSequenceAtStage{};
    std::uint64_t clientMessageSequenceAtStage{};
    std::uint64_t lastInputSequence{};
    std::uint32_t squadRow{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t spawnGeneration{};
    std::uint16_t slotIndex{};
    std::int32_t expectedAlive{};
    std::int32_t alive{};
    std::int32_t maximumObservedAlive{};
    /**
     * Members lost in this spawn generation, summed over the client's per-slot counts. Each was
     * created first, so it also bounds the members created from below.
     */
    std::int32_t maximumObservedLost{};
    bool generationKnown{};
    bool aliveKnown{};
};

/** Only present generation, alive and lost fields can update a population observation. */
struct SquadPopulationReport final {
    std::uint64_t attemptGeneration{};
    /** Link whose client sent the report. */
    std::uint64_t sourceGeneration{};
    std::uint64_t inputSequence{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t spawnGeneration{};
    std::uint16_t slotIndex{};
    std::int32_t alive{};
    std::int32_t lost{};
    bool generationKnown{};
    bool aliveKnown{};
    bool lostKnown{};
    bool initialized{};
    /**
     * The report came from the link that simulates the squad now, so its population follows it.
     * The mission runtime sends only admitted reports; another is a replica's and moves nothing.
     */
    bool admitted{};
};

/** Type-23 reports position, power and lock as three independent channels. */
inline constexpr std::size_t kDeviceChannelCount = 3;

/** Only fields present in an accepted report may advance a request join. */
struct DeviceReportedChannel final {
    float value{};
    std::int32_t sequence{-1};
    bool valueKnown{};
    bool sequenceKnown{};
    bool operator==(const DeviceReportedChannel&) const = default;
};

/** A device desire gains report eligibility only after its exact output is staged. */
struct DeviceRequestReport final {
    DeviceReportedChannel reported{};
    std::uint64_t requestKey{};
    std::uint64_t attemptGeneration{};
    std::uint64_t sourceGeneration{};
    std::uint64_t inputSequenceAtStage{};
    std::uint64_t clientMessageSequenceAtStage{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t slotRow{};
    std::uint16_t slotIndex{};
    std::uint8_t channel{};
    std::int16_t sequence{};
    float value{};
    bool applied{};
};

/** Accepted device deltas retain their input and attempt owners through deferred dispatch. */
struct DeviceReport final {
    std::array<DeviceReportedChannel, kDeviceChannelCount> channels{};
    std::uint64_t attemptGeneration{};
    std::uint64_t sourceGeneration{};
    std::uint64_t inputSequence{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint16_t slotIndex{};
    /**
     * The report came from the link that simulates the device now, so an unconfirmed request on it
     * follows that link. The mission runtime sends only admitted reports.
     */
    bool admitted{};
};

/** Only channels carried by a transported Auth body can renew a device report owner. */
struct DevicePublication final {
    std::array<DeviceReportedChannel, kDeviceChannelCount> channels{};
    /** Zero means no exact pending or retained Host body supplied the publication. */
    std::uint64_t originatingHostRevision{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint16_t slotIndex{};
};

/** The frame owns these input boundaries before its bytes reach transport. */
struct DevicePublicationBoundary final {
    std::uint64_t attemptGeneration{};
    std::uint64_t sourceGeneration{};
    std::uint64_t inputSequence{};
    std::uint64_t clientMessageSequence{};
    /** Every generation one authenticated link of the activity owned when the frame was built. */
    std::array<std::uint64_t, core::network_capacity::kActivityPlayers> liveGenerations{};
    std::uint8_t liveGenerationCount{};
};

/** Exact SDK program identity bound to one activity-session generation. */
struct ProgramKey final {
    std::array<std::byte, 32> sdkBuildSha256{};
    /** Domain-separated SHA-256 of the complete generated-world generation tuple. */
    std::array<std::byte, 32> worldGenerationSha256{};
    /** SHA-256 of the exact Lua bytes opened for this durable program. */
    std::array<std::byte, 32> scriptSourceSha256{};
    std::uint32_t activityDefinition{};
    std::uint32_t worldScenarioTag{};
    std::int16_t activityIndex{-1};
    bool publicTarget{};

    friend bool operator==(const ProgramKey&, const ProgramKey&) = default;
};

/** Closed typed actions that may leave Mission Lua. */
enum class IntentKind : std::uint8_t {
    placeSquad,
    activateAuthoredScene,
    setObjectActive,
    setDeviceChannel,
    applySlotAuth,
    setLifetime,
    fireTrigger,
    playSequence,
    setCinematicActive,
    resetObjectives,
    advanceTask,
    playDialogueCue,
    selectMissionState,
    bindCombatantToSquad,
    actorCommand,
    playPerformance,
    restartCheckpoint,
    signalAuthoredScene,
    stopAuthoredScene,
    playActorSequence,
    assignCombatObjective,
    runActorProgram,
    retireActor,
    setInteractableObject,
    watchDamage,
    setGhostLink,
    holdSpawn,
    /** Sends one msg-19 incident to the client; `firstRow` is the target, `authBody` the payload.
     */
    sendIncident,
    teleport,
    setActorControls,
    deliverPassengers,
    /** Raises or lowers one activity flag; `active` false lowers it. */
    setActivityFlag,
    /** Attaches or detaches one hop-on sensor; the Host owns its revision. */
    setHopOn,
    /** Sets an authored object-channel prefix; the Host owns its revisions. */
    setObjectChannels,
    /** Sets a type-70 audience; the Host owns its revision and raises it only to clear. */
    setEngagement,
};

/** Actor sequence values belong to one combatant and one SDK/client generation. */
struct ActorSequenceOwner final {
    std::array<std::byte, 32> sdkBuildSha256{};
    std::array<std::byte, 32> sdkPayloadSha256{};
    std::uint64_t activityClientGeneration{};
    std::uint64_t sessionId{};
    std::uint64_t sessionCreatedRevision{};
    std::uint32_t activityRow{};
    std::uint32_t scenarioRow{};
    std::uint32_t slotRow{};
    std::uint32_t actorClassRow{};
    bool operator==(const ActorSequenceOwner&) const = default;
};

/** One object a mission omits, named the way a roster group is: its tag and its registry key. */
struct MissionSeedOmission final {
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    bool operator==(const MissionSeedOmission&) const = default;
};

/** Typed action with RAII-owned body bytes and no packet or native pointers. */
struct TypedIntent final {
    std::uint32_t expectedObjectiveRevision{};
    bool objectiveReconsider{};
    bool objectiveReserved{};
    bool objectivePreserveReservation{true};
    bool objectiveRefreshAwareness{};
    std::uint64_t attemptGeneration{kFirstAttemptGeneration};
    ActorSequenceOwner sequenceOwner{};
    std::array<std::int32_t, kSquadMemberCapacity> squadCounts{};
    /** Objects the mission leaves out of the state it selects. */
    std::array<MissionSeedOmission, kMissionSeedOmitCapacity> seedOmissions{};
    /** Object slots activated with firstRow on one revision, so one push carries them all. */
    std::array<std::uint32_t, kIntentBurstCapacity> burstRows{};
    std::uint8_t burstRowCount{};
    std::uint8_t seedOmissionCount{};
    /** Exact body bytes of the typed Auth action, or the payload of `sendIncident`. */
    std::vector<std::byte> authBody{};
    std::array<std::byte, 32> sdkBuildSha256{};
    /** Nonzero key the effect call returned to the script, for effectResult correlation. */
    std::uint64_t requestKey{kAbsentIntentKey};
    IntentKind kind{IntentKind::placeSquad};
    std::uint32_t firstRow{};
    std::uint32_t secondRow{};
    /** The input a scene signal sends, or the one an activation starts with; zero is none. */
    std::uint32_t sceneEventKey{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t authSchema{};
    /** SDK-selected actor-command selector, never a wire constant owned by Mission State. */
    std::uint32_t actorCommandSelector{};
    /** Spawn set a checkpoint restart respawns at, or a teleport arrives at. */
    std::uint32_t checkpointSpawnHash{};
    /** Spawn set a state selection's arrival uses; zero keeps the program's declared set. */
    std::uint32_t arrivalSpawnHash{};
    /** Request key of the wipe a checkpoint release ends; zero arms one instead. */
    std::uint64_t checkpointReleaseRequest{};
    /** Authored effective region selected by the generated mission-state table. */
    std::int32_t effectiveRegion{-1};
    /** Catalog slot row of a type-66 rule the squad spawns at instead of its own; absent is -1. */
    std::int32_t squadSpawnRuleRow{-1};
    /** Catalog slot row of the type-60 volume a dialogue line waits for; absent is -1. */
    std::int32_t dialogueFilterRow{-1};
    /** Catalog slot row of the type-1 squad an object entry spawns at; absent is -1. */
    std::int32_t objectLocationRow{-1};
    std::int32_t entryIndex{};
    std::int32_t actorCommandValue{};
    std::uint32_t activityFlagKey{};
    /** A squad placement's temperament list, sent only when set; unset keeps the squad's last. */
    std::array<std::uint32_t, kSquadTemperamentCapacity> squadTemperaments{};
    std::uint8_t squadTemperamentCount{};
    bool squadTemperamentsSet{};
    /** A sequence's choice bit, or -1 for none. */
    std::int32_t sequenceChoice{-1};
    /** The sequence keeps running and only changes its choice. */
    bool sequenceRetain{};
    /** An object body keeps the committed generation and updates the live entity. */
    bool objectInPlace{};
    float deviceValue{};
    std::uint16_t authBitCount{};
    std::uint16_t slotIndex{};
    std::uint8_t squadMode{};
    bool squadRetireOnReturn{};
    std::uint8_t squadCount{};
    std::uint8_t deviceChannel{};
    std::uint8_t slotType{};
    std::uint16_t authByteCount{};
    std::uint8_t lifetimeState{};
    bool deviceSnap{};
    bool active{};
    /** An object activation also carries the no-change capture-plate component Auth. */
    bool capturePlate{};
    /** An object activation also carries the state-machine position lane (in place). */
    bool positionChild{};
    /** Requested normalized native position value. */
    float positionValue{};
    /** True to snap; false preserves the authored transition rate. */
    bool positionSnap{true};
    /** A state transition may end its captured map-prop lifetimes before teleporting. */
    bool retirePlacedProps{};
};

/** One durable action and the exact Host output revision assigned to it. */
struct PendingIntent final {
    TypedIntent value{};
    std::uint64_t sequence{};
    std::uint64_t missionRevision{};
    std::uint64_t hostOutputRevision{};
};

/** Bounded canonical name shared by durable variables and timers. */
struct StateKey final {
    std::array<char, kStateKeyByteCapacity> bytes{};
    std::uint8_t length{};
};

/** Closed durable value kinds accepted from Mission Lua. */
enum class VariableValueKind : std::uint8_t {
    boolean,
    integer,
    real,
    string,
};

/** Value-owned scalar retained without a Lua object or allocator dependency. */
struct VariableValue final {
    std::array<char, kVariableStringByteCapacity> stringValue{};
    std::int64_t integerValue{};
    double realValue{};
    std::uint16_t stringLength{};
    VariableValueKind kind{VariableValueKind::boolean};
    bool booleanValue{};
};

/** One canonical key/value row in the authoritative mission state. */
struct ScriptVariable final {
    StateKey key{};
    VariableValue value{};
};

/** One active authoritative timer keyed by name and an absolute service tick. */
struct MissionTimer final {
    StateKey key{};
    std::uint64_t deadlineTick{};
    std::uint64_t sequence{};
};

/** Server-owned mission state retained with one exact activity session. */
struct MissionState final {
    AttemptState attempt{};
    std::vector<SquadPopulation> squadPopulations{};
    std::vector<DeviceRequestReport> deviceRequests{};
    /** Durable delivery queue with no policy count limit. */
    std::vector<PendingIntent> pendingIntents{};
    std::array<ScriptVariable, kVariableCapacity> variables{};
    std::array<MissionTimer, kTimerCapacity> timers{};
    ProgramKey program{};
    std::uint64_t revision{};
    /** Last accepted binding-owned Host mission-input sequence. */
    std::uint64_t inputSequence{};
    /** Last sequence issued to an accepted Host input, including not-yet-committed rows. */
    std::uint64_t issuedInputSequence{};
    std::uint64_t nextIntentSequence{kFirstIntentSequence};
    /** Next request key the VM will mint, so keys stay unique across a restore. */
    std::uint64_t nextIntentKey{kFirstIntentKey};
    std::uint64_t nextTimerSequence{kFirstTimerSequence};
    std::uint32_t phase{};
    std::size_t variableCount{};
    std::size_t timerCount{};
    bool programBound{};
    bool started{};
    bool faulted{};
};

} // namespace sunrise::state::activity::mission
