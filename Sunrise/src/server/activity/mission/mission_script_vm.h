#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../state/activity_sdk/format.h"
#include "../host_runtime.h"
#include "mission_script_catalog_sdk.h"
#include "mission_script_manifest_sdk.h"
#include "mission_script_world_sdk.h"

namespace sunrise::server::activity::mission::lua_vm {

namespace detail {
struct VmAccess;
}

/** Lua ceiling per open program. The largest generated module peaks near 23 MiB. */
inline constexpr std::size_t kArenaByteCapacity = 64U * 1024U * 1024U;
inline constexpr std::size_t kVariableCapacity = state::activity::mission::kVariableCapacity;
inline constexpr std::size_t kTimerCapacity = state::activity::mission::kTimerCapacity;
inline constexpr std::size_t kStateKeyByteCapacity =
    state::activity::mission::kStateKeyByteCapacity;
/** One variable string may not outgrow the durable slot the state holds it in. */
inline constexpr std::size_t kVariableStringByteCapacity =
    state::activity::mission::kVariableStringByteCapacity;
/** Squad Auth accepts at most fifteen authored member counts. */
inline constexpr std::size_t kSquadMemberCapacity = 15;
/** Script source is bounded before it reaches the parser. */
inline constexpr std::size_t kSourceByteCapacity = 128U * 1024U;
/** One callback is stopped after this many approximate VM instructions. */
inline constexpr std::uint32_t kInstructionBudget = 5'000'000;
/** Generated mission modules are large declarations and execute only while a VM opens. */
inline constexpr std::uint32_t kInitializationInstructionBudget = 100'000'000;
/** Stable storage holds everything but the arena block, which the open program owns. */
inline constexpr std::size_t kVmStorageByteCapacity = 512U * 1024U;

/** Current generated identity selected by the runtime before a program attaches. */
struct ProgramIdentity final {
    std::array<char, 72> sdkBuildId{};
    std::array<char, 64> activityId{};
    /** The only filesystem search path available to generated-module require calls. */
    std::array<char, 1024> sdkLuaSearchPath{};
    std::uint32_t activityRow{};
    std::uint32_t definitionHash{};
    /** The player key the bound link's message 5 binds; the client matches it on its own datum. */
    std::uint64_t playerKey{};
    /** Exact ActivityClient directory selected for this VM. */
    bool publicTarget{};
};

/** One activity-local squad row resolved against the pinned SDK view. */
struct SquadDefinition final {
    std::array<std::int32_t, kSquadMemberCapacity> defaultCounts{};
    std::string_view id{};
    std::string_view name{};
    std::uint32_t nativeRow{};
    std::uint32_t localRow{};
    std::size_t memberCount{};
};

/** One activity-local authored-scene row resolved against the pinned SDK view. */
struct SceneDefinition final {
    std::array<char, 512> id{};
    std::uint32_t occurrenceRow{};
    std::uint32_t slotRow{};
    std::uint32_t localRow{};
    std::size_t idLength{};
};

/** One scenario-local reusable object slot resolved against the pinned SDK view. */
struct SlotDefinition final {
    std::string_view id{};
    std::string_view name{};
    std::string_view objectId{};
    std::string_view senseSchemaId{};
    std::string_view authSchemaId{};
    std::uint32_t nativeRow{};
    std::uint32_t localRow{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t slotIndex{};
    std::uint32_t slotType{};
    std::uint32_t componentClass{};
    std::uint32_t senseSchema{};
    std::uint32_t authSchema{};
    std::uint32_t flags{};
};

/**
 * One type-38 task-sensor target whose authored reference was proved to resolve.
 * The client calls through that reference without a null check, so an unresolved target is fatal.
 */
struct TaskSensorDefinition final {
    std::string_view id{};
    std::uint32_t localRow{};
    std::uint32_t slotRow{};
    /** Authored object key the slot record's reference resolves to. */
    std::uint32_t targetObjectKey{};
    /** Authored bit index the applier clears. It is authored, never a caller parameter. */
    std::uint32_t bitIndex{};
};

/**
 * One type-68 directive element proved against the authored directive list.
 * The client indexes by the element index with no bound, and dereferences the hash lookup with
 * no null check. So both must already have resolved before a caller can name this element.
 */
struct DirectiveElementDefinition final {
    std::string_view id{};
    std::uint32_t localRow{};
    std::uint32_t slotRow{};
    /** Name hash an authored directive owns. A hash owning nothing never becomes a handle. */
    std::uint32_t nameHash{};
    /** Index inside the owning authored sub-array, already inside its real length. */
    std::int32_t elementIndex{};
    /** Rows that sub-array holds, so the bound is the array's, never the wire lane's width. */
    std::uint32_t elementCount{};
};

/** One state name a type-42 performance sensor may start on the actor it drives. */
struct PerformanceStateDefinition final {
    std::uint32_t slotRow{};
    /** FNV-1 of the state name in the target actor's state-machine definition. */
    std::uint32_t nameHash{};
    /** Distinct state names every member of the target squad declares. */
    std::uint32_t stateCount{};
};

using ActorSequenceOwner = state::activity::mission::ActorSequenceOwner;

/** One extracted sequence entry; unsupported native kinds remain readable. */
struct ActorSequenceDefinition final {
    std::string_view id{};
    std::string_view name{};
    std::string_view symbol{};
    std::string_view sourcePath{};
    std::uint32_t catalogRow{};
    std::uint32_t keyHash{};
    std::uint32_t kind{};
    std::uint32_t resourceTag{};
    std::uint32_t tableIndex{};
    std::uint32_t ordinal{};
    std::uint32_t sourceOffset{};
    bool playable{};
};

/** These callbacks reread the exact actor owner before exposing or playing a value. */
struct ActorSequenceApi final {
    const void* context{};
    bool (*owner)(const void*, std::uint32_t slotRow, ActorSequenceOwner&) noexcept {};
    std::size_t (*count)(const void*, const ActorSequenceOwner&) noexcept {};
    bool (*resolve)(const void*,
                    const ActorSequenceOwner&,
                    std::uint32_t ordinal,
                    ActorSequenceDefinition&) noexcept {};
};

/** One peer session bound to this destination, as the roster currently holds it. */
struct PeerSession final {
    std::uint64_t sessionId{};
    /** Record generation, so a replacement is told apart from the record it replaced. */
    std::uint64_t sessionGeneration{};
    /** Peer client key bound by its join, or zero before one. */
    std::uint64_t memberKey{};
    /** The identity message 12 publishes for the peer, which is also its message-5 player key. */
    std::uint64_t joinIdentity{};
};

/** Peers the VM can report. It is the activity session capacity, not a bound of its own. */
inline constexpr std::size_t kPeerCapacity = state::activity::kSessionCapacity;

/** One generated typed Lua surface owned by an activity-message route. */
struct ActivityMessageSurfaceDefinition final {
    std::string_view id{};
    std::string_view luaName{};
};

/** Capacity follows the generated mission-surface section, not current msg-5 callability. */
inline constexpr std::size_t kActivityMessageSurfaceCapacity =
    state::activity_sdk::format::kMissionSurfaceCount;

/** One checked activity-message row resolved from the pinned SDK view. */
struct ActivityMessageDefinition final {
    std::array<ActivityMessageSurfaceDefinition, kActivityMessageSurfaceCapacity> ingressSurfaces{};
    std::array<ActivityMessageSurfaceDefinition, kActivityMessageSurfaceCapacity> egressSurfaces{};
    std::string_view name{};
    std::string_view ingressAdapter{};
    std::string_view ingressAdapterPath{};
    std::string_view ingressClass{};
    std::string_view egressAdapter{};
    std::string_view egressAdapterPath{};
    std::string_view egressClass{};
    std::string_view outputCodec{};
    std::string_view outputCodecPath{};
    std::string_view stateOwner{};
    std::string_view stateOwnerPath{};
    std::string_view luaExposure{};
    std::string_view ingressDelivery{};
    std::string_view egressDelivery{};
    std::string_view lateJoinHandoff{};
    std::string_view ingressStatus{};
    std::string_view egressStatus{};
    std::uint32_t localRow{};
    std::uint32_t messageId{};
    std::uint32_t direction{};
    std::uint32_t coverage{};
    std::uint32_t definitionHandle{};
    std::uint32_t callForm{};
    std::uint32_t definitionState{};
    std::uint32_t definitionStructSize{};
    std::uint32_t wireMinBits{};
    std::uint32_t wireMaxBits{};
    std::uint32_t fieldCount{};
    std::uint32_t namedFieldCount{};
    std::uint32_t graphFieldCount{};
    std::uint32_t authoredFieldCount{};
    std::uint32_t typedLuaSurfaceCount{};
    std::uint32_t communicationFlags{};
    std::uint32_t flags{};
    std::uint8_t ingressSurfaceCount{};
    std::uint8_t egressSurfaceCount{};
    bool executableRoute{};
};

/** One checked field row owned by an activity message in the pinned SDK view. */
struct ActivityMessageFieldDefinition final {
    std::string_view path{};
    std::string_view name{};
    std::string_view type{};
    std::uint32_t localRow{};
    std::uint32_t globalRow{};
    std::uint32_t messageRow{};
    std::uint32_t ordinal{};
    std::uint32_t source{};
    std::uint32_t structOffset{};
    std::uint32_t structOffsetAbs{};
    std::uint32_t typeCode{};
    std::int64_t bias{};
    std::uint32_t bits{};
    std::uint32_t bitsMin{};
    std::uint32_t bitsMax{};
    std::uint32_t widthOrCountOffset{};
    std::uint32_t repeat{};
    std::uint32_t nestedHandle{};
    std::uint32_t ownerHandle{};
    std::uint32_t depth{};
    std::uint32_t flags{};
    std::uint32_t confidence{};
};

/** Complete authenticated binding metadata for the activity owning this script VM. */
struct ActivityBindingDefinition final {
    std::string_view internalName{};
    std::string_view displayName{};
    std::uint32_t selectedActivityRootTag{};
    std::uint32_t selectedScenarioTag{};
    std::uint32_t matchmakingConfigTag{};
    std::uint32_t joinStatus{};
    std::uint32_t bindingDisposition{};
    std::uint32_t bindingReason{};
    std::uint32_t bindingEvidenceBasis{};
    std::uint32_t runnableStatus{};
    bool fullSdkAcceptable{};
    bool hasInternalName{};
    bool hasMatchmakingConfig{};
};

/** Which closed tag range one immutable ActivityView collection exposes. */
enum class ActivityBindingTagKind : std::uint8_t {
    activityRootCandidates,
    scenarioNameCandidates,
    evidenceRoots,
};

/** One exact package locator supporting the current activity classification. */
struct ActivityBindingLocatorDefinition final {
    std::uint32_t tag{};
    std::uint64_t offset{};
    std::uint32_t localRow{};
};

using ActionKind = state::activity::mission::IntentKind;

using StateKey = state::activity::mission::StateKey;
using VariableValueKind = state::activity::mission::VariableValueKind;
using VariableValue = state::activity::mission::VariableValue;
using ScriptVariable = state::activity::mission::ScriptVariable;
using MissionTimer = state::activity::mission::MissionTimer;

using IntentKind = ActionKind;

using ResolveSquadRow = bool (*)(const void* context,
                                 std::uint32_t localRow,
                                 SquadDefinition& output) noexcept;
using ResolveSquadId = bool (*)(const void* context,
                                std::string_view id,
                                SquadDefinition& output) noexcept;
using ResolveSceneRow = bool (*)(const void* context,
                                 std::uint32_t localRow,
                                 SceneDefinition& output) noexcept;
using ResolveSceneId = bool (*)(const void* context,
                                std::string_view id,
                                SceneDefinition& output) noexcept;
using ResolveSlotRow = bool (*)(const void* context,
                                std::uint32_t localRow,
                                SlotDefinition& output) noexcept;
using ResolveSlotId = bool (*)(const void* context,
                               std::string_view id,
                               SlotDefinition& output) noexcept;
using ResolveSenseSlot = bool (*)(const void* context,
                                  const host::SenseObservationKey& key,
                                  SlotDefinition& output) noexcept;
using ResolveTaskSensorRow = bool (*)(const void* context,
                                      std::uint32_t localRow,
                                      TaskSensorDefinition& output) noexcept;
using ResolveTaskSensorId = bool (*)(const void* context,
                                     std::string_view id,
                                     TaskSensorDefinition& output) noexcept;
using ResolveDirectiveElementRow = bool (*)(const void* context,
                                            std::uint32_t localRow,
                                            DirectiveElementDefinition& output) noexcept;
using ResolveDirectiveElement = bool (*)(const void* context,
                                         std::uint32_t slotRow,
                                         std::uint32_t nameHash,
                                         std::int32_t elementIndex,
                                         DirectiveElementDefinition& output) noexcept;
/** A zero name hash selects the one state the slot's target declares, and refuses any other. */
using ResolvePerformanceState = bool (*)(const void* context,
                                         std::uint32_t slotRow,
                                         std::uint32_t nameHash,
                                         PerformanceStateDefinition& output) noexcept;
using ResolveActivityMessageRow = bool (*)(const void* context,
                                           std::uint32_t localRow,
                                           ActivityMessageDefinition& output) noexcept;
using ResolveActivityMessageId = bool (*)(const void* context,
                                          std::uint32_t messageId,
                                          ActivityMessageDefinition& output) noexcept;
using ResolveActivityMessageName = bool (*)(const void* context,
                                            std::string_view name,
                                            ActivityMessageDefinition& output) noexcept;
using ResolveActivityMessageFieldRow = bool (*)(const void* context,
                                                std::uint32_t messageRow,
                                                std::uint32_t localRow,
                                                ActivityMessageFieldDefinition& output) noexcept;
using ResolveActivityMessageFieldIndex = bool (*)(const void* context,
                                                  std::uint32_t messageId,
                                                  std::uint32_t globalFieldIndex,
                                                  ActivityMessageFieldDefinition& output) noexcept;
using ResolveActivityBinding = bool (*)(const void* context,
                                        ActivityBindingDefinition& output) noexcept;
using ActivityBindingTagCount = std::size_t (*)(const void* context,
                                                ActivityBindingTagKind kind) noexcept;
using ResolveActivityBindingTag = bool (*)(const void* context,
                                           ActivityBindingTagKind kind,
                                           std::uint32_t localRow,
                                           std::uint32_t& output) noexcept;
using ActivityBindingLocatorCount = std::size_t (*)(const void* context) noexcept;
using ResolveActivityBindingLocator = bool (*)(const void* context,
                                               std::uint32_t localRow,
                                               ActivityBindingLocatorDefinition& output) noexcept;
using DefinitionCount = std::size_t (*)(const void* context) noexcept;

/** Native SDK/live projection used by the sandbox; no borrowed pointer is script-visible. */
struct DefinitionApi final {
    const void* context{};
    ResolveSquadRow resolveSquadRow{};
    ResolveSquadId resolveSquadId{};
    ResolveSceneRow resolveSceneRow{};
    ResolveSceneId resolveSceneId{};
    ResolveSlotRow resolveSlotRow{};
    ResolveSlotId resolveSlotId{};
    ResolveSenseSlot resolveSenseSlot{};
    // These resolvers expose generated task and directive rows without script-owned identifiers.
    ResolveTaskSensorRow resolveTaskSensorRow{};
    ResolveTaskSensorId resolveTaskSensorId{};
    ResolveDirectiveElementRow resolveDirectiveElementRow{};
    ResolveDirectiveElement resolveDirectiveElement{};
    ResolvePerformanceState resolvePerformanceState{};
    ActorSequenceApi actorSequences{};
    ResolveActivityMessageRow resolveActivityMessageRow{};
    ResolveActivityMessageId resolveActivityMessageId{};
    ResolveActivityMessageName resolveActivityMessageName{};
    ResolveActivityMessageFieldRow resolveActivityMessageFieldRow{};
    ResolveActivityMessageFieldIndex resolveActivityMessageFieldIndex{};
    ResolveActivityBinding resolveActivityBinding{};
    ActivityBindingTagCount activityBindingTagCount{};
    ResolveActivityBindingTag resolveActivityBindingTag{};
    ActivityBindingLocatorCount activityBindingLocatorCount{};
    ResolveActivityBindingLocator resolveActivityBindingLocator{};
    DefinitionCount squadCount{};
    DefinitionCount sceneCount{};
    DefinitionCount slotCount{};
    DefinitionCount taskSensorCount{};
    DefinitionCount directiveElementCount{};
    DefinitionCount activityMessageCount{};
    CatalogDefinitionApi catalog{};
    ManifestDefinitionApi manifest{};
    WorldDefinitionApi world{};
};

using Intent = state::activity::mission::TypedIntent;

/** Stable program-open outcome. */
enum class OpenStatus : std::uint8_t {
    ready,
    sourceTooLarge,
    outOfMemory,
    compileError,
    runtimeError,
    invalidProgram,
};

/** Stable protected-callback outcome. */
enum class CallStatus : std::uint8_t {
    committed,
    noHandler,
    inactive,
    scriptError,
    instructionBudget,
    outOfMemory,
};

/** Read-only diagnostics copied without entering Lua. */
struct Snapshot final {
    std::array<char, 256> lastError{};
    std::uint64_t stateRevision{};
    std::uint64_t callbacks{};
    std::uint64_t committedCallbacks{};
    std::uint64_t refusedCallbacks{};
    std::uint64_t collections{};
    std::uint32_t phase{};
    std::size_t pendingIntents{};
    std::size_t variableCount{};
    std::size_t timerCount{};
    std::uint64_t nextTimerSequence{state::activity::mission::kFirstTimerSequence};
    std::uint64_t nextIntentKey{state::activity::mission::kFirstIntentKey};
    std::int32_t initialStateRegion{-1};
    std::size_t arenaBytes{};
    std::size_t arenaHighWater{};
    std::size_t arenaBytesAfterClose{};
    bool active{};
    bool faulted{};
    bool hasInitialState{};
};

/** Noncopyable Lua state with a fixed arena and RAII-owned work queues. */
class Vm final {
public:
    Vm() noexcept;
    ~Vm() noexcept;
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

private:
    friend struct detail::VmAccess;
    friend OpenStatus
    open(Vm&, const ProgramIdentity&, const DefinitionApi&, std::span<const char>) noexcept;
    friend bool rebind(Vm&, const ProgramIdentity&, const DefinitionApi&) noexcept;
    friend bool restore_state(Vm&,
                              std::uint32_t,
                              std::uint64_t,
                              std::span<const ScriptVariable>,
                              std::span<const MissionTimer>,
                              std::uint64_t,
                              std::span<const Intent>) noexcept;
    friend CallStatus start(Vm&, std::uint64_t) noexcept;
    friend CallStatus load(Vm&, std::uint64_t) noexcept;
    friend CallStatus
    dispatch(Vm&, const host::Event&, const host::ClientMessageSnapshot*, std::uint64_t) noexcept;
    friend bool handles_event(const Vm&, host::EventKind) noexcept;
    friend bool initial_state_region(const Vm&, std::int32_t&) noexcept;
    friend bool pending_intent(const Vm&, Intent&) noexcept;
    friend bool snapshot_intents(const Vm&, std::vector<Intent>&) noexcept;
    friend bool snapshot_durable_state(const Vm&,
                                       std::span<ScriptVariable>,
                                       std::size_t&,
                                       std::span<MissionTimer>,
                                       std::size_t&,
                                       std::uint64_t&) noexcept;
    friend void consume_intent(Vm&) noexcept;
    friend void discard_intents(Vm&) noexcept;
    friend void fault(Vm&, std::string_view) noexcept;
    friend void snapshot(const Vm&, Snapshot&) noexcept;
    friend void close(Vm&) noexcept;

    alignas(std::max_align_t) std::array<std::byte, kVmStorageByteCapacity> storage_{};
};

/** Opens one text-only, build-matched program in a fresh sandbox. */
[[nodiscard]] OpenStatus open(Vm& vm,
                              const ProgramIdentity& identity,
                              const DefinitionApi& definitions,
                              std::span<const char> source) noexcept;

/**
 * Re-points an open program at a new bound view of the same source.
 * @return False when no program is open.
 */
[[nodiscard]] bool
rebind(Vm& vm, const ProgramIdentity& identity, const DefinitionApi& definitions) noexcept;
/** Restores authoritative state and typed actions before the first callback. */
[[nodiscard]] bool restore_state(Vm& vm,
                                 std::uint32_t phase,
                                 std::uint64_t revision,
                                 std::span<const ScriptVariable> variables,
                                 std::span<const MissionTimer> timers,
                                 std::uint64_t nextTimerSequence,
                                 std::uint64_t nextIntentKey,
                                 std::span<const Intent> pendingIntents) noexcept;
/** Restores authoritative state when no typed actions are pending. */
[[nodiscard]] bool restore_state(Vm& vm, std::uint32_t phase, std::uint64_t revision) noexcept;
/**
 * Publishes the current peer set so any callback can read it, on_load included.
 * It is instance state, so it persists until the next publish. Peers past kPeerCapacity are
 * dropped, which is the same bound the roster itself carries.
 */
void publish_peers(Vm& vm, std::span<const PeerSession> peers) noexcept;
/** Runs the optional on_start(context, state) transaction. */
[[nodiscard]] CallStatus start(Vm& vm, std::uint64_t now = 0) noexcept;
/**
 * Runs optional on_load(context, state) after same-session VM reattachment.
 * It rebuilds only Lua-heap caches and reserves no revision.
 */
[[nodiscard]] CallStatus load(Vm& vm, std::uint64_t now = 0) noexcept;
/** Runs the optional callback for the event's exact native kind. */
[[nodiscard]] CallStatus dispatch(Vm& vm,
                                  const host::Event& event,
                                  const host::ClientMessageSnapshot* clientMessage = nullptr,
                                  std::uint64_t now = 0) noexcept;
/** @return True when the program declares the exact event callback. */
[[nodiscard]] bool handles_event(const Vm& vm, host::EventKind kind) noexcept;
/** Reads the optional effective region captured from program.initial_state.region_index. */
[[nodiscard]] bool initial_state_region(const Vm& vm, std::int32_t& output) noexcept;
/** Copies the oldest committed action without consuming it. */
[[nodiscard]] bool pending_intent(const Vm& vm, Intent& output) noexcept;
/** Copies the complete ordered outbox without consuming an action. */
[[nodiscard]] bool snapshot_intents(const Vm& vm, std::vector<Intent>& output) noexcept;
/** Copies complete durable variable and timer candidates without invoking Lua. */
[[nodiscard]] bool snapshot_durable_state(const Vm& vm,
                                          std::span<ScriptVariable> variables,
                                          std::size_t& variableCount,
                                          std::span<MissionTimer> timers,
                                          std::size_t& timerCount,
                                          std::uint64_t& nextTimerSequence,
                                          std::uint64_t& nextIntentKey) noexcept;
/** Removes the oldest action after a native adapter accepted it. */
void consume_intent(Vm& vm) noexcept;
/** Discards every committed action after a terminal native-delivery failure. */
void discard_intents(Vm& vm) noexcept;
/** Stops callbacks after an unrecoverable event or action failure. */
void fault(Vm& vm, std::string_view reason) noexcept;
/** Copies bounded diagnostics without invoking script code. */
void snapshot(const Vm& vm, Snapshot& output) noexcept;
/** Closes one state and releases every allocation back to its fixed arena. */
void close(Vm& vm) noexcept;

[[nodiscard]] const char* status_name(OpenStatus status) noexcept;
[[nodiscard]] const char* status_name(CallStatus status) noexcept;

} // namespace sunrise::server::activity::mission::lua_vm
