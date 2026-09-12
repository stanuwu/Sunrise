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
/** A mission leaves at most eight objects out of the seed it selects. */
inline constexpr std::size_t kMissionSeedOmitCapacity = 8;
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
};

/** Typed action with RAII-owned body bytes and no packet or native pointers. */
struct TypedIntent final {
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
    std::uint32_t secondRow{};
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
