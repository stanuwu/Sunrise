#pragma once

#include "../common/common_sync.h"
#include "middleware/gameplay/external/external_entity_codec.h"
#include "runtime.h"

namespace sunrise::server::gameplay::physics::replication {

/** Type-specific state selected before a generic planner result is encoded. */
struct EntityPayloadPlan final {
    middleware::gameplay::external::TypePayload baseline{};
    middleware::gameplay::external::TypePayload update{};
    middleware::gameplay::external::EntityToken anchor{};
    middleware::gameplay::external::EntityType type{
        middleware::gameplay::external::EntityType::sobject};
    bool anchorPresent{};
};

/** Builds one immutable external frame without committing packet outcomes. */
[[nodiscard]] bool
build_external_plan(const common::CommonPlan* commonPlan,
                    const Plan* entityPlan,
                    const EntityPayloadPlan* payloadPlan,
                    middleware::gameplay::external::ExternalEntityFrame& frame) noexcept;

} // namespace sunrise::server::gameplay::physics::replication
