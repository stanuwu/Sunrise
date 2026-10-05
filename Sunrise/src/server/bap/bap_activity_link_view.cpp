/** Read-only views of the activity link a binding owns, taken under the session lock. */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>

#include "../../state/activity/runtime.h"
#include "encrypted/push/activity/internal.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::server::bap {
namespace {

/**
 * Tests the world-package identity used by the HUD and generated scenario layout.
 * Reason, nonce, activity index and descriptor are binding identity, so two selections naming one
 * loaded destination may differ in all of them.
 */
[[nodiscard]] bool
same_destination_package(const state::activity::destination::DestinationSelection& left,
                         const state::activity::destination::DestinationSelection& right) noexcept {
    return left.packageNameLength != 0 && left.packageNameLength == right.packageNameLength
           && left.packageName == right.packageName;
}

/** Resolves the region used by this exact connection's msg-5 builder. */
[[nodiscard]] encrypted::push::activity::EffectiveRegion
selected_region_locked(const Session& session) noexcept {
    const auto base = encrypted::push::activity::effective_region(session.activity.session);
    return encrypted::push::activity::selected_effective_region(session, base.arrival);
}

} // namespace

/** Region index this connection's msg-5 builder selects. */
std::int32_t selected_region_index_locked(const Session& session) noexcept {
    return selected_region_locked(session).index;
}

/** Counts matching authenticated links while the caller already owns the BAP lock. */
std::size_t activity_link_count_locked(const state::activity::SessionBinding& binding) noexcept {
    std::size_t count = 0;
    static_cast<void>(unique_activity_link_locked(binding, count));
    return count;
}

/** Counts authenticated BAP links that currently own one exact activity generation. */
std::size_t activity_link_count(const state::activity::SessionBinding& binding) noexcept {
    const std::shared_lock lock(session_lock());
    const std::size_t count = activity_link_count_locked(binding);
    return count;
}

/** Reads the unique ActivityClient and the exact region its msg-5 builder will use. */
bool activity_link_view(const state::activity::SessionBinding& binding,
                        ActivityLinkView& output) noexcept {
    output = {};
    const std::shared_lock lock(session_lock());
    const Session* const session = unique_activity_link_locked(binding, output.matchingLinks);
    if (session != nullptr) {
        const auto region = selected_region_locked(*session);
        output.activityClientGeneration = session->activity.bindingGeneration;
        output.effectiveRegion = region.index;
        output.sliceSetIndex =
            state::activity::membership::reported_slice_set(session->activity.session.sessionId);
        output.arrivalSliceSetIndex = static_cast<std::int32_t>(region.arrival);
        output.effectiveRegionReported = region.reported;
        output.joined = session->activity.role == ActivityClientRole::publicTarget
                        || session->activityJoinGeneration == session->activity.bindingGeneration;
        output.publicTarget = session->activity.role == ActivityClientRole::publicTarget;
        output.rosterReason = session->activityRosterReason;
        output.playerKey = encrypted::push::activity::published_player_key(*session);
    }
    return session != nullptr;
}

/** Selects the exact live ActivityClient for the client's local world slice. */
bool current_activity_link_view(std::int32_t localSliceSet,
                                CurrentActivityLinkView& output) noexcept {
    output = {};
    const Session* only = nullptr;
    const Session* matched = nullptr;
    const Session* coherent = nullptr;
    const Session* privateCurrent = nullptr;
    bool oneDestination = true;
    const std::shared_lock lock(session_lock());
    for (const Session& session : sessions()) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0
            || !state::activity::binding_matches(session.activity.session)) {
            continue;
        }
        only = &session;
        ++output.activeLinks;
        if (session.activity.role == ActivityClientRole::privateCurrent
            && (privateCurrent == nullptr
                || session.activity.bindingGeneration
                       > privateCurrent->activity.bindingGeneration)) {
            privateCurrent = &session;
        }
        if (coherent == nullptr) {
            coherent = &session;
        } else {
            oneDestination = oneDestination
                             && same_destination_package(coherent->activity.session.destination,
                                                         session.activity.session.destination);
            // Prefer the persistent private-current link for destination metadata. Public targets
            // are disposable region views and can overlap while the client changes bubbles.
            if ((session.activity.role == ActivityClientRole::privateCurrent
                 && coherent->activity.role != ActivityClientRole::privateCurrent)
                || (session.activity.role == coherent->activity.role
                    && session.activity.bindingGeneration > coherent->activity.bindingGeneration)) {
                coherent = &session;
            }
        }
        if (localSliceSet >= 0 && selected_region_locked(session).index == localSliceSet) {
            ++output.matchingRegions;
            // A public region may be represented by several overlapping links. Prefer the stable
            // private-current owner; within one role, use the newest binding.
            const bool moreSpecific =
                matched == nullptr
                || (session.activity.role == ActivityClientRole::privateCurrent
                    && matched->activity.role != ActivityClientRole::privateCurrent)
                || (session.activity.role == matched->activity.role
                    && session.activity.bindingGeneration > matched->activity.bindingGeneration);
            if (moreSpecific) {
                matched = &session;
            }
        }
    }
    const Session* const selected = privateCurrent != nullptr ? privateCurrent
                                    : matched != nullptr      ? matched
                                    : output.activeLinks == 1 ? only
                                    : oneDestination          ? coherent
                                                              : nullptr;
    if (selected != nullptr) {
        output.binding = selected->activity.session;
        output.activityClientGeneration = selected->activity.bindingGeneration;
        output.effectiveRegion = selected_region_locked(*selected).index;
        output.publicTarget = selected->activity.role == ActivityClientRole::publicTarget;
    }
    return selected != nullptr;
}

/** Reads one exact ActivityClient's common-root inputs. */
bool activity_replication_view(const state::activity::SessionBinding& binding,
                               ActivityReplicationView& output) noexcept {
    output = {};
    const std::shared_lock lock(session_lock());
    std::size_t count = 0;
    const Session* const session = unique_activity_link_locked(binding, count);
    const bool ready =
        session != nullptr && session->activityPatchEpoch.seen
        && session->activityPatchEpoch.bindingGeneration == session->activity.bindingGeneration;
    if (ready) {
        output.binding = session->activity.session;
        output.patchEpoch = session->activityPatchEpoch.value;
        output.activityClientGeneration = session->activity.bindingGeneration;
        output.groupSessionId = session->activity.groupSessionId;
        output.memberId = session->activityMemberKey;
        output.replicationEpoch = session->activity.replicationEpoch;
    }
    return ready;
}

/** Reads the unique ActivityClient whose activity session is the supplied view token. */
bool activity_replication_view_for_session(std::uint64_t activitySessionId,
                                           ActivityReplicationView& output) noexcept {
    output = {};
    if (activitySessionId == 0) {
        return false;
    }
    const std::shared_lock lock(session_lock());
    const Session* selected = nullptr;
    std::size_t count = 0;
    for (const Session& session : sessions()) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0
            || session.activity.session.sessionId != activitySessionId
            || !state::activity::binding_matches(session.activity.session)
            || !session.activityPatchEpoch.seen
            || session.activityPatchEpoch.bindingGeneration != session.activity.bindingGeneration) {
            continue;
        }
        selected = &session;
        ++count;
    }
    if (count == 1 && selected != nullptr) {
        output.binding = selected->activity.session;
        output.patchEpoch = selected->activityPatchEpoch.value;
        output.activityClientGeneration = selected->activity.bindingGeneration;
        output.groupSessionId = selected->activity.groupSessionId;
        output.memberId = selected->activityMemberKey;
        output.replicationEpoch = selected->activity.replicationEpoch;
    }
    return count == 1;
}

/** Reads the unique ActivityClient bound to one gameplay group session. */
bool activity_replication_view_for_group(std::uint64_t groupSessionId,
                                         ActivityReplicationView& output) noexcept {
    output = {};
    if (groupSessionId == 0) {
        return false;
    }
    const std::shared_lock lock(session_lock());
    const Session* selected = nullptr;
    std::size_t count = 0;
    for (const Session& session : sessions()) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0
            || session.activity.groupSessionId != groupSessionId || !session.activityPatchEpoch.seen
            || session.activityPatchEpoch.bindingGeneration != session.activity.bindingGeneration) {
            continue;
        }
        selected = &session;
        ++count;
    }
    if (count == 1 && selected != nullptr) {
        output.binding = selected->activity.session;
        output.patchEpoch = selected->activityPatchEpoch.value;
        output.activityClientGeneration = selected->activity.bindingGeneration;
        output.groupSessionId = selected->activity.groupSessionId;
        output.memberId = selected->activityMemberKey;
        output.replicationEpoch = selected->activity.replicationEpoch;
    }
    return count == 1;
}

} // namespace sunrise::server::bap
