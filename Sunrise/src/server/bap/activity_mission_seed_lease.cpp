#include "activity_mission_seed_lease.h"

namespace sunrise::server::bap {

/** Validates and clears stale state on one already-selected ActivityClient session. */
ActivityMissionSeedLeaseStatus mission_seed_session_status(
    Session& session, std::uint32_t scenarioRow, std::uint64_t expectedGeneration) noexcept {
    if (expectedGeneration == 0 || session.activity.bindingGeneration != expectedGeneration) {
        return ActivityMissionSeedLeaseStatus::staleActivityClient;
    }
    MissionSeedLease& lease = session.activityMissionSeed;
    if (lease.configured && lease.bindingGeneration != session.activity.bindingGeneration) {
        lease = {};
    }
    if (lease.configured && lease.plan.scenarioRow != scenarioRow) {
        return ActivityMissionSeedLeaseStatus::wrongScenario;
    }
    return ActivityMissionSeedLeaseStatus::ready;
}

/** Copies one validated session lease into the public read-only view. */
void read_mission_seed_lease(const Session& session,
                             std::size_t matchingLinks,
                             ActivityMissionSeedLeaseView& output) noexcept {
    const MissionSeedLease& lease = session.activityMissionSeed;
    output = {};
    output.plan = lease.configured ? lease.plan : ActivityMissionSeedPlan{};
    output.matchingLinks = matchingLinks;
    output.activityClientGeneration = session.activity.bindingGeneration;
    output.revision = lease.revision;
    output.publishedRevision = lease.publishedRevision;
    output.configured = lease.configured;
    output.publicationPending = lease.configured && lease.revision != lease.publishedRevision;
    output.publicationRefused =
        output.publicationPending && lease.refusedRevision == lease.revision;
    output.regionArrivalPending = lease.configured && lease.regionArrivalPending;
}

} // namespace sunrise::server::bap
