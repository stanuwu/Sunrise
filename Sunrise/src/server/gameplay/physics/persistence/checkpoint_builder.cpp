#include "checkpoint_builder.h"

namespace sunrise::server::gameplay::physics::persistence {

/** Builds one complete checkpoint without changing output on failure. */
CheckpointBuildResult build_checkpoint(const CheckpointBuildRequest& request,
                                       const world::WorldSnapshot& worldSnapshot,
                                       const MechanicsCheckpoint& mechanics,
                                       const world::IActivityPolicy& policy,
                                       Checkpoint& checkpoint) noexcept {
    const world::ActivityPolicyManifest manifest = policy.manifest();
    if (request.checkpointId == 0 || request.logicalWorldId == 0 || request.contentBuildId == 0
        || !world::valid_blueprint(manifest.policy) || manifest.persistenceSchemaVersion == 0
        || manifest.fixedRateHz == 0 || manifest.fixedRateHz > world::kMaximumFixedRateHz
        || (manifest.contentBuildId != 0 && manifest.contentBuildId != request.contentBuildId)
        || !world::valid_world(worldSnapshot.identity) || !worldSnapshot.healthy
        || worldSnapshot.canonicalHash == 0
        || worldSnapshot.canonicalHash != world::compute_snapshot_hash(worldSnapshot)) {
        return CheckpointBuildResult::invalid;
    }

    Checkpoint candidate{};
    candidate.world = worldSnapshot;
    candidate.mechanics = mechanics;
    candidate.policy = manifest.policy;
    candidate.checkpointId = request.checkpointId;
    candidate.logicalWorldId = request.logicalWorldId;
    candidate.contentBuildId = request.contentBuildId;
    candidate.deterministicRngState = request.deterministicRngState;
    candidate.journalCursor = request.journalCursor;
    candidate.schemaVersion = manifest.persistenceSchemaVersion;
    candidate.fixedRateHz = manifest.fixedRateHz;
    PolicyBlobWriter writer(candidate.policyState);
    if (!policy.save(writer)) {
        return CheckpointBuildResult::policyRejected;
    }
    candidate.canonicalHash = compute_checkpoint_hash(candidate);
    if (!valid_checkpoint(candidate)) {
        return CheckpointBuildResult::invalid;
    }
    checkpoint = candidate;
    return CheckpointBuildResult::built;
}

} // namespace sunrise::server::gameplay::physics::persistence
