#include "bap_connection_publication.h"

#include <Windows.h>

namespace sunrise::server::bap::encrypted {
namespace {

/** Measured delay before the second Family-4 snapshot. */
constexpr std::uint64_t kFamily4RepushDelayMs = 400;
/** The banner pair lands the same unsolicited way and hits the same record-state race. */
constexpr std::uint64_t kBannerRepushDelayMs = 400;
/**
 * How long the roster keeps its faster cadence after a load starts.
 * The slice-set load step costs 9.2 to 14.1 s, so this covers it.
 */
constexpr std::uint64_t kTransitionWindowMs = 15'000;

} // namespace

/** Captures the connection fields one service outcome carries. */
ConnectionFields connection_fields(const ServiceOutcome& outcome) noexcept {
    ConnectionFields fields{};
    if (!outcome.hasActivityTransaction) {
        return fields;
    }
    const auto& plan = outcome.activityPlan;
    if (plan.delivery == activity_message::Delivery::joinNotifications) {
        fields.joinMemberKey = plan.entitySlotMutation.memberKey;
        fields.joinCharacterSoid = plan.joinCharacterSoid;
        fields.joinsActivity = true;
    }
    // The initial load is a transition too, and its token does not arrive for several seconds.
    fields.opensTransitionWindow =
        plan.delivery == activity_message::Delivery::joinNotifications || plan.transitionStarted;
    if (plan.mutationDomain == activity_message::MutationDomain::patchEpoch) {
        fields.patchEpoch = plan.patchEpoch;
        fields.retainsPatchEpoch = true;
    }
    return fields;
}

/** Publishes the captured connection fields after a successful commit. */
void publish_connection_fields(Session& session,
                               const transactions::Publication& publication,
                               const ConnectionFields& fields) noexcept {
    if (publication.hasActivitySessionBinding) {
        session.activitySessionId = publication.activitySessionId;
    }
    if (fields.joinMemberKey != 0) {
        session.activityMemberKey = fields.joinMemberKey;
    }
    if (fields.joinCharacterSoid != 0) {
        session.activityCharacterSoid = fields.joinCharacterSoid;
    }
    if (fields.retainsPatchEpoch) {
        session.activityPatchEpoch = fields.patchEpoch;
        session.activityPatchEpochSeen = true;
    }
    if (fields.opensTransitionWindow) {
        session.activityTransitionUntilTick = GetTickCount64() + kTransitionWindowMs;
    }
    // A join resets the client's roster container, so the warm-up is re-armed. Its unconditional
    // state-byte moves make the client deactivate and rebuild every roster-owned object, and the
    // player object binds to the published membership only on that rebuild.
    if (fields.joinsActivity) {
        session.activityRosterSends = 0;
        session.activityRosterGroups = 0;
    }
}

/** Arms the owed Family-4 and banner re-pushes when the queuez publication asks for them. */
void arm_repushes(Session& session, const queuez::StagedPublication& queuezPublication) noexcept {
    if (!queuezPublication.armsFamily4Repush || queuezPublication.family4RepushRoot == 0) {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    session.family4RepushDueTick = now + kFamily4RepushDelayMs;
    session.family4RepushRoot = queuezPublication.family4RepushRoot;
    session.family4RepushArmed = true;
    session.bannerRepushDueTick = now + kBannerRepushDelayMs;
    session.bannerRepushRoot = queuezPublication.family4RepushRoot;
    session.bannerRepushArmed = true;
}

} // namespace sunrise::server::bap::encrypted
