#pragma once

#include "../world/world_types.h"
#include "definition.h"

namespace sunrise::server::gameplay::physics::replication {

/** One logical actor's last accepted projection into replication fields. */
struct WorldProjectionState final {
    world::WorldIdentity world{};
    world::ActorSnapshot actor{};
    state::gameplay::physics::Allocation allocation{};
    FieldVersions versions{};
    std::uint64_t combatRevision{};
    bool initialized{};
};

/** Bounded result of one canonical world-to-replication projection. */
enum class ProjectionResult : std::uint8_t {
    created,
    updated,
    unchanged,
    invalid,
};

/** Projects one canonical actor without changing state on rejected input. */
[[nodiscard]] ProjectionResult project_actor(const world::WorldIdentity& worldIdentity,
                                             const world::ActorSnapshot& actor,
                                             const state::gameplay::physics::Allocation& allocation,
                                             std::uint64_t combatRevision,
                                             WorldProjectionState& state,
                                             ActorSnapshot& output) noexcept;

} // namespace sunrise::server::gameplay::physics::replication
