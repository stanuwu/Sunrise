#include <algorithm>
#include <cmath>
#include <limits>

#include "../../middleware/bap/activity_message/squad_objective_auth.h"
#include "../../state/activity/runtime.h"
#include "host_runtime_actor_program.h"
#include "host_runtime_counter_auth.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace scene = middleware::bap::activity_message::sensor_auth_update;
namespace squad = middleware::bap::activity_message::squad_auth;
using namespace detail;

} // namespace

namespace detail {

/** @return True when one carried group contains the target's exact selected auth slot. */
[[nodiscard]] bool
valid_state_local_group(const ScriptableTarget& target,
                        const state::build_data::scenarios::RosterGroup& group) noexcept {
    const std::size_t slot = target.rosterSlotOffset;
    return target.stateLocalRoster && target.stateLocalRegion >= 0
           && target.rosterGroupIndex == kGeneratedRosterGroupIndex
           && target.sdkObjectIndex != kNoSdkObjectIndex
           && state::build_data::scenarios::valid_roster_group(group) && group.objectTag != 0
           && group.objectTag == target.objectTag && group.registryKey == target.registryKey
           && slot < group.slotCount && group.slotTypes[slot] == target.slotType
           && group.slotIndices[slot] == target.slotIndex
           && (group.slotFlags[slot] & state::build_data::scenarios::kSlotAuthFlag) != 0;
}

} // namespace detail

/** A scene may prepare absent actors and must preserve every active incompatible binding. */
ActorSquadBindingStatus actor_squad_binding_status(const state::activity::SessionBinding& binding,
                                                   const ScriptableTarget& actor) noexcept {
    if (actor.slotType != auth::kType2SlotType || actor.authSchema != auth::kType2Schema
        || !state::activity::binding_matches(binding)) {
        return ActorSquadBindingStatus::incompatible;
    }
    AcquireSRWLockShared(&g_lock);
    const Instance* instance = find_instance(binding);
    auto status = ActorSquadBindingStatus::incompatible;
    if (instance != nullptr && instance->view.active) {
        const auto retained = std::find_if(instance->scriptableAuthEstate.begin(),
                                           instance->scriptableAuthEstate.end(),
                                           [&actor](const auto& row) noexcept {
                                               return row.target.objectTag == actor.objectTag
                                                      && row.target.registryKey == actor.registryKey
                                                      && row.target.slotIndex == actor.slotIndex
                                                      && row.target.slotType == actor.slotType
                                                      && row.target.authSchema == actor.authSchema;
                                           });
        if (retained == instance->scriptableAuthEstate.end()) {
            status = ActorSquadBindingStatus::missing;
        } else {
            auth::Type2ProgramLayout layout{};
            if (retained->byteCount != 0 && retained->byteCount <= retained->body.size()
                && auth::inspect_type2_program(std::span(retained->body).first(retained->byteCount),
                                               retained->bitCount,
                                               layout)) {
                if (!layout.enabled) {
                    status = ActorSquadBindingStatus::missing;
                } else if (layout.bindingWire == 2 || layout.bindingWire == 4) {
                    status = ActorSquadBindingStatus::ready;
                }
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return status;
}

/** A source check reads only transported activity state while the Host lock is held. */
ActorProgramSourceStatus actor_program_source_status(const state::activity::SessionBinding& binding,
                                                     const ScriptableTarget& source,
                                                     PendingScriptableOverride* output) noexcept {
    if (output != nullptr) {
        *output = {};
    }
    if (source.slotType != squad::kSlotType || source.authSchema != squad::kSchema
        || !state::activity::binding_matches(binding)) {
        return ActorProgramSourceStatus::incompatible;
    }
    ScriptableRequest request{};
    request.programSource = source;
    AcquireSRWLockShared(&g_lock);
    const Instance* instance = find_instance(binding);
    auto status = ActorProgramSourceStatus::incompatible;
    if (instance != nullptr && instance->view.active) {
        const auto* retained = actor_program::retained(*instance, source);
        status = ActorProgramSourceStatus::missing;
        if (retained != nullptr) {
            if (output != nullptr) {
                *output = *retained;
            }
            if (actor_program::source_ready(*instance, request)) {
                status = ActorProgramSourceStatus::ready;
            } else {
                namespace patch = middleware::bap::activity_message::mission_auth_patch;
                patch::Layout layout{};
                const auto body = std::span(retained->body).first(retained->byteCount);
                middleware::encoding::bits::Reader reader(body);
                std::uint64_t active = 0;
                if (!patch::parse(squad::kSchema, body, retained->bitCount, layout)
                    || !reader.skip(layout.fields[actor_program::kActiveField].offset)
                    || !reader.read(static_cast<std::uint8_t>(
                                        patch::kSquadRules[actor_program::kActiveField].width),
                                    active)
                    || active != actor_program::kInactiveWire) {
                    status = ActorProgramSourceStatus::incompatible;
                }
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return status;
}

/** Queues a program template whose published revisions belong to the retained Host state. */
bool request_type2_program(const state::activity::SessionBinding& binding,
                           const ScriptableTarget& target,
                           const ScriptableTarget& source,
                           const state::build_data::scenarios::RosterGroup* group,
                           std::span<const std::byte> body,
                           std::uint16_t bits,
                           bool spawn,
                           std::uint64_t expectedActivityClientGeneration,
                           const ScriptableOutputReservation* reservation,
                           bool retire) noexcept {
    ScriptableRequest request{};
    if ((spawn && retire) || target.slotType != auth::kType2SlotType
        || target.authSchema != auth::kType2Schema || body.size() > request.authBody.size()
        || !auth::validate_type2_body(body, bits) || expectedActivityClientGeneration == 0
        || (spawn && (source.slotType != squad::kSlotType || source.authSchema != squad::kSchema))
        || (target.stateLocalRoster
            && (group == nullptr || !valid_state_local_group(target, *group)))) {
        return false;
    }
    request.binding = binding;
    request.target = target;
    request.programSource = source;
    if (group != nullptr) {
        request.stateLocalRosterGroup = *group;
    }
    request.kind = retire ? ScriptableOverrideKind::combatantRetirement
                          : ScriptableOverrideKind::combatantProgram;
    request.active = spawn;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.authBitCount = bits;
    request.authByteCount = static_cast<std::uint16_t>(body.size());
    std::copy(body.begin(), body.end(), request.authBody.begin());
    return enqueue_request(request, reservation);
}

/** Queues an SDK-validated objective decision without accepting a script revision. */
bool request_squad_objective(const state::activity::SessionBinding& binding,
                             const ScriptableTarget& target,
                             const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
                             const state::activity::mission::TypedIntent& decision,
                             std::uint16_t objectiveIndex,
                             std::uint64_t expectedActivityClientGeneration,
                             const ScriptableOutputReservation& reservation) noexcept {
    namespace objective = middleware::bap::activity_message::squad_objective;
    if (target.slotType != squad::kSlotType || target.authSchema != objective::kSchema
        || objectiveIndex > scene::kMaximumSlotIndex
        || decision.entryIndex < objective::kNoTaskGroup
        || decision.entryIndex >= objective::kTaskGroupCount
        || expectedActivityClientGeneration == 0
        || (target.stateLocalRoster
            && (stateLocalRosterGroup == nullptr
                || !valid_state_local_group(target, *stateLocalRosterGroup)))) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    if (stateLocalRosterGroup != nullptr) {
        request.stateLocalRosterGroup = *stateLocalRosterGroup;
    }
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.objectiveIndex = objectiveIndex;
    request.entryIndex = decision.entryIndex;
    request.expectedObjectiveRevision = decision.expectedObjectiveRevision;
    request.objectiveReconsider = decision.objectiveReconsider;
    request.objectivePreserveReservation = decision.objectivePreserveReservation;
    request.objectiveReserved = decision.objectiveReserved;
    request.objectiveRefreshAwareness = decision.objectiveRefreshAwareness;
    request.kind = ScriptableOverrideKind::squadObjective;
    return enqueue_request(request, &reservation);
}

/** Queues one generation-bound type-23 update for an exact package-derived ClientRef. */
bool request_type23_override(const state::activity::SessionBinding& binding,
                             const ScriptableTarget& target,
                             auth::Type23Channel channel,
                             float value,
                             bool snap,
                             std::uint64_t expectedActivityClientGeneration,
                             const ScriptableOutputReservation* reservation) noexcept {
    const auto channelIndex = static_cast<std::size_t>(channel);
    if (target.slotType != auth::kType23SlotType || target.authSchema != auth::kType23Schema
        || channelIndex >= auth::kType23ChannelCount || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.channel = channel;
    request.value = value;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::type23;
    request.snap = snap;
    return enqueue_request(request, reservation);
}

/** Queues one package-owned type-4 entry transition. */
bool request_state_local_type4_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t entryIndex,
    bool active,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation,
    const ScriptableOutputReservation* burstHead) noexcept {
    if (target.slotType != auth::kType4SlotType || target.authSchema != auth::kType4Schema
        || !valid_state_local_group(target, stateLocalRosterGroup) || entryIndex < 0
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.entryIndex = entryIndex;
    request.active = active;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::object;
    if (burstHead != nullptr) {
        // Every body in one burst answers under the revision the head reserved for all of them.
        request.burstMember = true;
        request.expectedRevision = burstHead->revision;
        request.expectedIntentSequence = burstHead->intentSequence;
    }
    return enqueue_request(request, burstHead != nullptr ? nullptr : reservation);
}

/** Queues one retained named actor-channel write. */
bool request_state_local_type2_channel_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint32_t channelHash,
    float value,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType2SlotType || target.authSchema != auth::kType2Schema
        || !valid_state_local_group(target, stateLocalRosterGroup) || !std::isfinite(value)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.channelHash = channelHash;
    request.value = value;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::combatantChannel;
    return enqueue_request(request, reservation);
}

/** Queues only a sequence hash; the reducer owns the atom generation and retained body. */
bool request_state_local_type2_sequence(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint32_t sequenceHash,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType2SlotType || target.authSchema != auth::kType2Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || sequenceHash == middleware::bap::activity_message::kEmptyNameHash
        || sequenceHash == (std::numeric_limits<std::uint32_t>::max)()
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.sequenceHash = sequenceHash;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::combatantSequence;
    return enqueue_request(request, reservation);
}

/** Queues squad-member binding for one exact generated type-2 combatant. */
bool request_state_local_type2_squad_binding(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType2SlotType || target.authSchema != auth::kType2Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::combatantBinding;
    return enqueue_request(request, reservation);
}

/** Queues one generation-bound type-23 update from an exact generated roster group. */
bool request_state_local_type23_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    auth::Type23Channel channel,
    float value,
    bool snap,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    const auto channelIndex = static_cast<std::size_t>(channel);
    if (target.slotType != auth::kType23SlotType || target.authSchema != auth::kType23Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || channelIndex >= auth::kType23ChannelCount || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.channel = channel;
    request.value = value;
    request.kind = ScriptableOverrideKind::type23;
    request.snap = snap;
    return enqueue_request(request, reservation);
}

/** Queues one type-31 pulse for an exact package-derived ClientRef. */
bool request_type31_override(const state::activity::SessionBinding& binding,
                             const ScriptableTarget& target,
                             const ScriptableOutputReservation* reservation,
                             bool enabled) noexcept {
    if (target.slotType != auth::kType31SlotType || target.authSchema != auth::kType31Schema) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.kind = ScriptableOverrideKind::type31;
    request.triggerEnabled = enabled;
    return enqueue_request(request, reservation);
}

/** Queues one generation-bound type-31 pulse from an exact generated roster group. */
bool request_state_local_type31_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation,
    bool enabled) noexcept {
    if (target.slotType != auth::kType31SlotType || target.authSchema != auth::kType31Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::type31;
    request.triggerEnabled = enabled;
    return enqueue_request(request, reservation);
}

/** Queues one state-local sequence override, but only for an exact type-5 target. */
bool request_state_local_sequence_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType5SlotType || target.authSchema != auth::kType5Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::sequence;
    return enqueue_request(request, reservation);
}

/** Queues one state-local cinematic override, but only for an exact type-5 target. */
bool request_state_local_cinematic_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    bool active,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType6SlotType || target.authSchema != auth::kType6Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::cinematic;
    request.active = active;
    return enqueue_request(request, reservation);
}

/** Queues one type-42 performance start; the encoder assigns the rising generation. */
bool request_state_local_performance_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint32_t stateNameHash,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType42SlotType || target.authSchema != auth::kType42Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || expectedActivityClientGeneration == 0 || stateNameHash == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::performance;
    request.nameHash = stateNameHash;
    return enqueue_request(request, reservation);
}

/** Queues one objective reset from an exact generated roster group. */
bool request_state_local_objective_reset(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType3SlotType || target.authSchema != auth::kType3Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::objectiveReset;
    return enqueue_request(request, reservation);
}

/** Queues one generation-bound authored-task change from an exact generated roster group. */
bool request_state_local_task_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType38SlotType || target.authSchema != auth::kType38Schema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::task;
    return enqueue_request(request, reservation);
}

/** Queues one generation-bound authored-scene activation from an exact generated roster group. */
bool request_state_local_authored_scene_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation,
    const AuthoredSceneDependencies& dependencies,
    std::uint32_t eventKey,
    bool stop) noexcept {
    if (target.slotType != scene::kAuthoredSceneSlotType
        || target.authSchema != scene::kAuthoredSceneAuthSchema
        || !valid_state_local_group(target, stateLocalRosterGroup)
        || !scene::valid_authored_scene_dependencies(dependencies)
        || eventKey == (std::numeric_limits<std::uint32_t>::max)() || (stop && eventKey != 0)
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = stop            ? ScriptableOverrideKind::authoredSceneStop
                   : eventKey == 0 ? ScriptableOverrideKind::authoredScene
                                   : ScriptableOverrideKind::authoredSceneEvent;
    request.sceneEventKey = eventKey;
    request.sceneDependencies = dependencies;
    return enqueue_request(request, reservation);
}

/** Queues one SDK-bounded dialogue line pulse from an exact generated roster group. */
bool request_state_local_dialogue_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint16_t cueIndex,
    std::uint16_t authoredCueCount,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation,
    auth::Type2LaneClientRef filter) noexcept {
    const bool filterValid =
        filter.slotIndex < 0
        || (filter.slotType == auth::kType53FilterSlotType
            && filter.registryKey
                   != middleware::bap::activity_message::auth_fields::kClientRefAbsentKey);
    if (target.slotType != auth::kType53SlotType || target.authSchema != auth::kType53Schema
        || !valid_state_local_group(target, stateLocalRosterGroup) || authoredCueCount == 0
        || authoredCueCount > auth::kType53EntryCount || cueIndex >= authoredCueCount
        || expectedActivityClientGeneration == 0 || !filterValid) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.dialogueCue = cueIndex;
    request.dialogueFilter = filter;
    request.kind = ScriptableOverrideKind::dialogue;
    return enqueue_request(request, reservation);
}

/** Queues one squad placement intent for an exact package-derived ClientRef. */
bool request_squad_override(const state::activity::SessionBinding& binding,
                            const ScriptableTarget& target,
                            const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
                            std::span<const std::int32_t> requestedCounts,
                            squad::Mode mode,
                            std::uint64_t expectedActivityClientGeneration,
                            std::optional<std::uint32_t> nameHash,
                            const ScriptableOutputReservation* reservation,
                            std::array<std::int8_t, 4> authoredProfile,
                            state::gameplay::squad_entity_retirement::Eligibility squadRetirement,
                            std::optional<squad::Destination> destination,
                            std::optional<squad::SpawnRule> spawnRule) noexcept {
    if (squadRetirement.enabled
        && (squadRetirement.squad.key != target.registryKey
            || squadRetirement.squad.index != target.slotIndex
            || squadRetirement.squad.type != target.slotType || squadRetirement.rsatTag == 0
            || squadRetirement.bubble >= 64
            || !std::ranges::any_of(requestedCounts, [](auto count) { return count > 0; }))) {
        return false;
    }
    if (target.slotType != squad::kSlotType || target.authSchema != squad::kSchema
        || (target.stateLocalRoster
                ? stateLocalRosterGroup == nullptr
                      || !valid_state_local_group(target, *stateLocalRosterGroup)
                : stateLocalRosterGroup != nullptr)
        || requestedCounts.size() < squad::kMinimumRequestedCountLength
        || requestedCounts.size() > squad::kMaximumRequestedCountLength
        || expectedActivityClientGeneration == 0 || !squad::valid_mode(mode)
        || !std::ranges::all_of(requestedCounts, [](std::int32_t count) { return count >= 0; })) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    if (stateLocalRosterGroup != nullptr) {
        request.stateLocalRosterGroup = *stateLocalRosterGroup;
    }
    std::ranges::copy(requestedCounts, request.requestedCounts.begin());
    request.requestedCountLength = requestedCounts.size();
    request.squadAuthoredProfile = authoredProfile;
    request.squadRetirement = squadRetirement;
    request.nameHash = nameHash;
    request.squadDestination = destination;
    request.squadSpawnRule = spawnRule;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.squadMode = mode;
    request.kind = ScriptableOverrideKind::squad;
    return enqueue_request(request, reservation);
}

/** Queues one generation-bound activity lifetime state through the serialized output slot. */
bool request_lifetime_override(const state::activity::SessionBinding& binding,
                               std::uint8_t lifetimeState,
                               std::uint64_t expectedActivityClientGeneration,
                               const ScriptableOutputReservation* reservation) noexcept {
    // Above the highest jump-table entry the client's spawn gate jumps out of its image.
    if (lifetimeState > kMaximumLifetimeState || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = ScriptableOverrideKind::lifetime;
    request.lifetimeState = lifetimeState;
    return enqueue_request(request, reservation);
}

/** Queues one exact SDK-compiled Auth body for a generation-bound ClientRef. */
bool request_sdk_auth_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
    std::span<const std::byte> body,
    std::uint16_t bitCount,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation,
    ScriptableOverrideKind kind,
    std::optional<SquadAttachmentOwnership> attachment) noexcept {
    if ((kind != ScriptableOverrideKind::sdkAuth && !counter_auth::compatible(kind, target))
        || body.empty() || body.size() > scene::kAuthOverrideByteCapacity
        || body.size() > (std::numeric_limits<std::uint16_t>::max)()
        || expectedActivityClientGeneration == 0 || !valid_auth_storage(body, bitCount)
        || (target.stateLocalRoster
                ? stateLocalRosterGroup == nullptr
                      || !valid_state_local_group(target, *stateLocalRosterGroup)
                : stateLocalRosterGroup != nullptr)) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    if (stateLocalRosterGroup != nullptr) {
        request.stateLocalRosterGroup = *stateLocalRosterGroup;
    }
    std::copy(body.begin(), body.end(), request.authBody.begin());
    request.authBitCount = bitCount;
    request.authByteCount = static_cast<std::uint16_t>(body.size());
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.kind = kind;
    request.squadAttachment = attachment;
    return enqueue_request(request, reservation);
}

} // namespace sunrise::server::activity::host
