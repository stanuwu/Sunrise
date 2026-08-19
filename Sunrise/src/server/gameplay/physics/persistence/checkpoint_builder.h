#pragma once

#include "checkpoint.h"

namespace sunrise::server::gameplay::physics::persistence {

/** Durable metadata supplied by the activity coordinator at one committed tick. */
struct CheckpointBuildRequest final {
    std::uint64_t checkpointId{};
    std::uint64_t logicalWorldId{};
    std::uint64_t contentBuildId{};
    std::uint64_t deterministicRngState{};
    std::uint64_t journalCursor{};
};

/** Bounded result of one transactional checkpoint build. */
enum class CheckpointBuildResult : std::uint8_t {
    built,
    invalid,
    policyRejected,
};

/** Builds one complete checkpoint without changing output on failure. */
[[nodiscard]] CheckpointBuildResult build_checkpoint(const CheckpointBuildRequest& request,
                                                     const world::WorldSnapshot& worldSnapshot,
                                                     const MechanicsCheckpoint& mechanics,
                                                     const world::IActivityPolicy& policy,
                                                     Checkpoint& checkpoint) noexcept;

} // namespace sunrise::server::gameplay::physics::persistence
