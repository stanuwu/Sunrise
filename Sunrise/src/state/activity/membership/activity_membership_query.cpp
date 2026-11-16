#include "activity_membership_query.h"

#include <Windows.h>

#include <algorithm>
#include <limits>

#include "../../account/account_context.h"
#include "../../runtime/storage/internal.h"
#include "../member_context.h"
#include "../member_selection.h"
#include "../transactions/internal.h"
#include "mission_party.h"

namespace sunrise::state::activity::membership {
namespace {
/** Service work outside a client scope reads the primary simulation row. */
template <class Record>
auto selected_member(Record& record) noexcept -> decltype(member_state(record, 0)) {
    if (!record.sharedMembers) {
        return &record.membership;
    }
    const auto context = member_context();
    const auto account = bound_account();
    if (context.sessionId == 0 && account == kLocalAccount) {
        return member_state(record, 0);
    }
    return member_state(record,
                        member_row(record,
                                   context.sessionId == record.sessionId ? context.memberKey : 0,
                                   account_primary_soid(account)));
}
/** @return True when an armed teleport names the region the client now holds. */
[[nodiscard]] bool teleport_arrived(const MembershipState& membership) noexcept {
    return membership.hasHostTeleport && membership.hostTeleport.state == kHostTeleportArmedState
           && membership.currentRegion.index == membership.hostTeleport.sliceSetIndex;
}

} // namespace

bool snapshot_mission_party(const SessionBinding& binding,
                            MissionParty& output,
                            std::size_t& count) noexcept {
    output = {};
    count = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    bool found = false;
    for (const auto& record : runtime::storage::g_state.activity.sessions) {
        if (!transactions::record_matches(record, binding)) {
            continue;
        }
        count = read_mission_party(record, output);
        found = true;
        break;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

bool arm_party_hard_wipe(const SessionBinding& binding,
                         std::uint64_t request,
                         std::int32_t region,
                         std::uint32_t spawn,
                         std::span<const MissionMember> expected) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    bool accepted = false;
    for (auto& record : runtime::storage::g_state.activity.sessions) {
        if (!transactions::record_matches(record, binding)) {
            continue;
        }
        accepted = same_mission_party(record, expected)
                   && arm_mission_party(record, request, region, spawn);
        break;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return accepted;
}

bool release_party_hard_wipe(const SessionBinding& binding, std::uint64_t request) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    bool accepted = false;
    for (auto& record : runtime::storage::g_state.activity.sessions) {
        if (!transactions::record_matches(record, binding)) {
            continue;
        }
        accepted = release_mission_party(record, request);
        break;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return accepted;
}

std::uint32_t current_revision(const SessionBinding& binding) noexcept {
    if (binding.sessionId == kAbsentSessionId || binding.createdRevision == 0) {
        return kAbsentRevision;
    }
    std::uint32_t revision = kAbsentRevision;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, binding.sessionId);
    if (target != kInvalidSessionSlot) {
        const auto& record = state.sessions[target];
        const auto* member = selected_member(record);
        if (record.joined && record.createdRevision == binding.createdRevision && member != nullptr
            && member->hasIdentity) {
            revision = member->revision;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return revision;
}

/** Tests whether the client has applied the current membership revision. */
bool acknowledged(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool applied = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        const MembershipState& membership = *selected_member(state.sessions[target]);
        applied = membership.revision != kAbsentRevision
                  && membership.acknowledgedRevision == membership.revision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return applied;
}

namespace {

/** Arms or clears the host-named teleport on one member row. @return True when it changed. */
bool arm_member_teleport(MembershipState& membership,
                         std::int32_t sliceSetIndex,
                         std::uint32_t spawnSetHash) noexcept {
    if (sliceSetIndex == kAbsentSliceSetIndex) {
        const bool changed = membership.hasHostTeleport;
        membership.hasHostTeleport = false;
        membership.hostTeleport = {};
        return changed;
    }
    if (membership.hasHostTeleport && membership.hostTeleport.sliceSetIndex == sliceSetIndex
        && membership.hostTeleport.spawnSetHash == spawnSetHash) {
        return false;
    }
    // Step 0 latches the token, it does not compare it, so the increment is bookkeeping for the
    // client's own arm. The state is what gates the step, and zero is idle.
    const std::uint8_t token = static_cast<std::uint8_t>(membership.hostTeleport.token + 1U);
    membership.hostTeleport.sliceSetIndex = sliceSetIndex;
    membership.hostTeleport.spawnSetHash = spawnSetHash;
    membership.hostTeleport.token = token;
    membership.hostTeleport.state = kHostTeleportArmedState;
    membership.hasHostTeleport = true;
    // The roster's spawn override decides where the player arrives, so it must name the same
    // set. A move that names none leaves the arrival to the roster's own choice.
    const bool namesSet = spawnSetHash != 0 && spawnSetHash != kEmptyNameHash;
    membership.hostSpawnRegion = namesSet ? sliceSetIndex : kAbsentRegionIndex;
    membership.hostSpawnSetHash = namesSet ? spawnSetHash : 0;
    // The client refuses a region record whose per-member token does not equal its own
    // transition count. Anticipate the next transition until its native report arrives; that
    // report replaces the prediction, including a request consumed by initial load.
    const std::uint8_t current =
        membership.hasTransitionToken ? membership.transitionToken : kInitialTransitionToken;
    auto advanced = static_cast<std::uint8_t>(current + 1U);
    if (advanced == 0) {
        advanced = kInitialTransitionToken;
    }
    membership.transitionToken = advanced;
    membership.hasTransitionToken = true;
    return true;
}

} // namespace

/** Arms or clears the host-named teleport region for one joined session. */
bool arm_host_teleport(std::uint64_t sessionId,
                       std::int32_t sliceSetIndex,
                       std::uint32_t spawnSetHash) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool changed = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        changed = arm_member_teleport(
            *selected_member(state.sessions[target]), sliceSetIndex, spawnSetHash);
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return changed;
}

/** Arms the same host-named teleport on every member of one joined session. */
bool arm_party_host_teleport(std::uint64_t sessionId,
                             std::int32_t sliceSetIndex,
                             std::uint32_t spawnSetHash) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool changed = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    for (std::size_t row = 0;
         target != kInvalidSessionSlot && row < entity_slots::kMemberLeaseRowCount;
         ++row) {
        if (auto* member = member_state(state.sessions[target], row)) {
            changed = arm_member_teleport(*member, sliceSetIndex, spawnSetHash) || changed;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return changed;
}

/** @return The spawn set the last host teleport named for this region, or zero. */
std::uint32_t host_spawn_set(std::uint64_t sessionId, std::int32_t region) noexcept {
    std::uint32_t result = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        const MembershipState& membership = *selected_member(state.sessions[target]);
        if (region >= 0 && membership.hostSpawnRegion == region) {
            result = membership.hostSpawnSetHash;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return result;
}

/** @return True while any member of the session has a hard wipe armed. */
bool hard_wipe_active(std::uint64_t sessionId) noexcept {
    bool active = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    for (std::size_t row = 0;
         target != kInvalidSessionSlot && !active && row < entity_slots::kMemberLeaseRowCount;
         ++row) {
        const auto* member = member_state(state.sessions[target], row);
        active = member != nullptr && member->hardWipe.active;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return active;
}

/** @return True while the client has not yet mirrored the armed wipe's spawn block. */
bool hard_wipe_needs_publish(std::uint64_t sessionId) noexcept {
    bool pending = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        const MembershipState& member = *selected_member(state.sessions[target]);
        const HardWipeState& wipe = member.hardWipe;
        pending = wipe.active
                  && (wipe.host.state == kHardWipeReleaseState
                      || member.spawn.opaqueByte != wipe.host.opaqueByte
                      || member.spawn.state < kHardWipeStartState);
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return pending;
}

/** @return True while the client has not yet mirrored an armed wipe's start block. */
bool hard_wipe_start_unseen(std::uint64_t sessionId) noexcept {
    bool unseen = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        const MembershipState& member = *selected_member(state.sessions[target]);
        const HardWipeState& wipe = member.hardWipe;
        unseen = wipe.active && wipe.host.state != kHardWipeReleaseState
                 && (member.spawn.opaqueByte != wipe.host.opaqueByte
                     || member.spawn.state < kHardWipeStartState);
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return unseen;
}

/** @return The checkpoint spawn set a wipe named for this region, or zero. */
std::uint32_t checkpoint_spawn_hash(std::uint64_t sessionId, std::int32_t region) noexcept {
    std::uint32_t result = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        const HardWipeState& wipe = selected_member(state.sessions[target])->hardWipe;
        if (wipe.requestKey != 0 && wipe.region == region) {
            result = wipe.spawnSetHash;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return result;
}

/** Holds or releases the spawn the mission program owns. */
void set_program_spawn_hold(std::uint64_t sessionId, bool held) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].membership.programSpawnHold = held;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** @return True while the mission program holds this session's spawn. */
bool program_spawn_hold(std::uint64_t sessionId) noexcept {
    bool held = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        held = state.sessions[target].membership.programSpawnHold;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return held;
}

/** Records the declared initial-state region on the joined session that owns the program. */
void note_declared_initial_region(std::uint64_t sessionId, std::int32_t region) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].membership.declaredInitialRegion = region;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Records the spawn set the attached program declared on the joined session that owns it. */
void note_declared_spawn_set(std::uint64_t sessionId, std::uint32_t spawnSetHash) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].membership.declaredSpawnSetHash = spawnSetHash;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Records the declared seed omissions on the joined session that owns the program. */
void note_declared_seed_omissions(
    std::uint64_t sessionId,
    std::span<const activity::mission::MissionSeedOmission> omissions) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        MembershipState& membership = state.sessions[target].membership;
        membership.declaredSeedOmissions = {};
        membership.declaredSeedOmissionCount = 0;
        if (omissions.size() <= membership.declaredSeedOmissions.size()) {
            std::copy(omissions.begin(), omissions.end(), membership.declaredSeedOmissions.begin());
            membership.declaredSeedOmissionCount = static_cast<std::uint8_t>(omissions.size());
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** @return The declared seed omissions copied into `output`, or zero while none fit. */
std::size_t
declared_seed_omissions(std::uint64_t sessionId,
                        std::span<activity::mission::MissionSeedOmission> output) noexcept {
    std::size_t count = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& membership = state.sessions[target].membership;
        if (membership.declaredSeedOmissionCount <= output.size()) {
            count = membership.declaredSeedOmissionCount;
            std::copy_n(membership.declaredSeedOmissions.begin(), count, output.begin());
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return count;
}

/** @return The declared spawn set, or zero while no program declared one. */
std::uint32_t declared_spawn_set(std::uint64_t sessionId) noexcept {
    std::uint32_t spawnSetHash = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        spawnSetHash = state.sessions[target].membership.declaredSpawnSetHash;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return spawnSetHash;
}

/** @return The declared initial-state region, or -1 while no program declared one. */
std::int32_t declared_initial_region(std::uint64_t sessionId) noexcept {
    std::int32_t region = kAbsentRegionIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const auto target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        region = state.sessions[target].membership.declaredInitialRegion;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return region;
}

/** @return The current activity State revision. */
std::uint64_t state_revision() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const std::uint64_t revision = runtime::storage::g_state.activity.stateRevision;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return revision;
}

/** Reports whether a host-named teleport is still waiting for the client to move. */
bool host_teleport_armed(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool armed = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        const MembershipState& membership = *selected_member(state.sessions[target]);
        // Once the client names the region as its pending or current leg it has the move, and
        // the arm still rides every body, so it must not keep forcing revisions.
        const std::int32_t region = membership.hostTeleport.sliceSetIndex;
        armed = membership.hasHostTeleport
                && membership.hostTeleport.state != kHostTeleportSpawnState
                && membership.region.index != region && membership.currentRegion.index != region;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return armed;
}

/** Raises an armed host teleport to its spawn state once the client holds the region it names. */
bool raise_host_teleport_spawn(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    // Every delivered roster asks, and almost none finds an arrived teleport.
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& current = runtime::storage::g_state.activity;
    const std::size_t row = activity::transactions::find_session(current, sessionId);
    const bool arrived = row != kInvalidSessionSlot
                         && selected_member(current.sessions[row]) != nullptr
                         && teleport_arrived(*selected_member(current.sessions[row]));
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (!arrived) {
        return false;
    }
    bool raised = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr
        && teleport_arrived(*selected_member(state.sessions[target]))) {
        // The client's step 3 runs the spawn only while the host state reads 3. Step 0 refuses
        // to re-latch on 3, so raising the arm cannot start a second move.
        selected_member(state.sessions[target])->hostTeleport.state = kHostTeleportSpawnState;
        raised = true;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return raised;
}

bool host_teleport_in_flight(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool inFlight = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        inFlight = selected_member(state.sessions[target])->hasHostTeleport;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return inFlight;
}

/** Reads the pending region the client last reported. */
std::int32_t reported_region(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return kAbsentRegionIndex;
    }
    std::int32_t region = kAbsentRegionIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        region = selected_member(state.sessions[target])->region.index;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return region;
}

/** Reads the region the client is in: the current leg, else the pending one. */
std::int32_t player_region(std::uint64_t sessionId) noexcept {
    const ClientPlacement placement = reported_placement(sessionId);
    return placement.currentRegion >= 0 ? placement.currentRegion : placement.region;
}

/** Reads the slice set from the client's newest D6 teleport state. */
std::int32_t reported_slice_set(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return kAbsentSliceSetIndex;
    }
    std::int32_t sliceSet = kAbsentSliceSetIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        sliceSet = selected_member(state.sessions[target])->teleport.sliceSetIndex;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return sliceSet;
}

/** Reads the client's region legs and current bubble together. */
ClientPlacement reported_placement(std::uint64_t sessionId) noexcept {
    ClientPlacement placement{};
    if (sessionId == kAbsentSessionId) {
        return placement;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr) {
        const MembershipState& membership = *selected_member(state.sessions[target]);
        placement.region = membership.region.index;
        placement.currentRegion = membership.currentRegion.index;
        placement.bubble = membership.bubble;
        placement.bubbleRevision = membership.bubbleRevision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return placement;
}

/** Names the region the client has instantiated. */
std::int32_t instantiated_region(const ClientPlacement& placement) noexcept {
    return placement.currentRegion >= 0 ? placement.currentRegion : kAbsentRegionIndex;
}

/** Reads the newest session the client has reported a region on. */
std::uint64_t live_region_session(std::uint64_t fallback) noexcept {
    std::uint64_t newest = kAbsentSessionId;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const SessionRecord& record : runtime::storage::g_state.activity.sessions) {
        const auto* member = selected_member(record);
        if (record.occupied && member && record.sessionId > newest
            && (member->region.index > kAbsentRegionIndex
                || member->currentRegion.index > kAbsentRegionIndex)) {
            newest = record.sessionId;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return newest == kAbsentSessionId ? fallback : newest;
}

/** Reads the member key the client joined this activity session under. */
std::uint64_t member_key(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return 0;
    }
    std::uint64_t key = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr
        && selected_member(state.sessions[target])->hasIdentity) {
        key = selected_member(state.sessions[target])->identity.memberKey;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return key;
}

/** Reads the identity value message 12 publishes at member record `+16`. */
std::uint64_t join_identity(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return 0;
    }
    std::uint64_t identity = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && selected_member(state.sessions[target]) != nullptr
        && selected_member(state.sessions[target])->hasIdentity) {
        identity = selected_member(state.sessions[target])->identity.joinIdentity;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return identity;
}

} // namespace sunrise::state::activity::membership
