#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
/** A compiled Slot Auth body must fit the widest bounded msg-5 table. */
inline constexpr std::size_t kMaximumAuthBodyByteCount = (53'150U + 7U) / 8U;
/** The msg-5 biased slot-type field carries native values from zero through 126. */
inline constexpr std::uint8_t kMaximumAuthSlotType = 126;
/** The msg-5 biased slot-index field wraps above this native value. */
inline constexpr std::uint16_t kMaximumAuthSlotIndex = 32767;
/** Above the highest jump-table entry the client's spawn gate jumps out of its image. */
inline constexpr std::uint8_t kMaximumIntentLifetimeState = 10;
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

/** Count evidence belongs to one transported placement and its echoed spawn generation. */
struct SquadPopulation final {
    std::uint64_t requestKey{};
    std::uint64_t attemptGeneration{};
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
    /** Members ever created in this spawn generation, summed over the client's per-slot counts. */
    std::int32_t maximumObservedCreated{};
    bool generationKnown{};
    bool aliveKnown{};
};

/** Only present generation, alive and created fields can update a population observation. */
struct SquadPopulationReport final {
    std::uint64_t attemptGeneration{};
    std::uint64_t inputSequence{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t spawnGeneration{};
    std::uint16_t slotIndex{};
    std::int32_t alive{};
    std::int32_t created{};
    bool generationKnown{};
    bool aliveKnown{};
    bool createdKnown{};
    bool initialized{};
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
    setSquadAttachment,
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
    /** Exact body bytes used only by the generic typed Auth action. */
    std::vector<std::byte> authBody{};
    std::array<std::byte, 32> sdkBuildSha256{};
    /** Nonzero key the effect call returned to the script, for effectResult correlation. */
    std::uint64_t requestKey{kAbsentIntentKey};
    IntentKind kind{IntentKind::placeSquad};
    std::uint32_t firstRow{};
    /** placeSquad with active=true: director SDK row; slotIndex is its authored objective ordinal.
     */
    std::uint32_t secondRow{};
    /** Placement-only exact SDK slot row; all other intent kinds leave this absent. */
    std::uint32_t spawnRuleSlotRow{0xFFFFFFFFU};
    /** Attachment source lifetime observed from an exact type-1 Sense baseline. */
    std::uint64_t sourceSpawnGeneration{};
    std::uint32_t sceneEventKey{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t authSchema{};
    /** SDK-selected actor-command selector, never a wire constant owned by Mission State. */
    std::uint32_t actorCommandSelector{};
    /** Spawn set a checkpoint restart respawns at. */
    std::uint32_t checkpointSpawnHash{};
    /** Request key of the wipe a checkpoint release ends; zero arms one instead. */
    std::uint64_t checkpointReleaseRequest{};
    /** Authored effective region selected by the generated mission-state table. */
    std::int32_t effectiveRegion{-1};
    /** Catalog slot row of a type-66 rule the squad spawns at instead of its own; absent is -1. */
    std::int32_t squadSpawnRuleRow{-1};
    /** Catalog slot row of the type-60 volume a dialogue line waits for; absent is -1. */
    std::int32_t dialogueFilterRow{-1};
    std::int32_t entryIndex{};
    std::int32_t actorCommandValue{};
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
