#include <algorithm>
#include <cmath>
#include <limits>

#include "../../state/activity/runtime.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace scene = middleware::bap::activity_message::sensor_auth_update;
namespace squad = middleware::bap::activity_message::squad_auth;
using namespace detail;

} // namespace

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
                             const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType31SlotType || target.authSchema != auth::kType31Schema) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.kind = ScriptableOverrideKind::type31;
    return enqueue_request(request, reservation);
}

/** Queues one generation-bound type-31 pulse from an exact generated roster group. */
bool request_state_local_type31_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint64_t expectedActivityClientGeneration,
    const ScriptableOutputReservation* reservation) noexcept {
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
    const ScriptableOutputReservation* reservation) noexcept {
    if (target.slotType != auth::kType53SlotType || target.authSchema != auth::kType53Schema
        || !valid_state_local_group(target, stateLocalRosterGroup) || authoredCueCount == 0
        || authoredCueCount > auth::kType53EntryCount || cueIndex >= authoredCueCount
        || expectedActivityClientGeneration == 0) {
        return false;
    }
    ScriptableRequest request{};
    request.binding = binding;
    request.target = target;
    request.stateLocalRosterGroup = stateLocalRosterGroup;
    request.expectedActivityClientGeneration = expectedActivityClientGeneration;
    request.dialogueCue = cueIndex;
    request.kind = ScriptableOverrideKind::dialogue;
    return enqueue_request(request, reservation);
}

/** Queues one squad placement intent for an exact package-derived ClientRef. */
bool request_squad_override(
    const state::activity::SessionBinding& binding,
    const ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
    std::span<const std::int32_t> requestedCounts,
    squad::Mode mode,
    std::uint64_t expectedActivityClientGeneration,
    std::optional<std::uint32_t> nameHash,
    const ScriptableOutputReservation* reservation,
    std::array<std::int8_t, 4> authoredProfile,
    state::gameplay::squad_entity_retirement::Eligibility squadRetirement) noexcept {
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
    const ScriptableOutputReservation* reservation) noexcept {
    if (body.empty() || body.size() > scene::kAuthOverrideByteCapacity
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
    request.kind = ScriptableOverrideKind::sdkAuth;
    return enqueue_request(request, reservation);
}

} // namespace sunrise::server::activity::host
