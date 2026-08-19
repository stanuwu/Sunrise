#include "external_plan.h"

namespace sunrise::server::gameplay::physics::replication {
namespace {

namespace external = middleware::gameplay::external;

/** @return True when one common plan is complete and count-one. */
[[nodiscard]] bool valid_common(const common::CommonPlan& plan) noexcept {
    return plan.prepared && plan.stateGeneration != 0 && plan.common.entryCount == 1
           && plan.common.entries[0].activitySessionId != 0;
}

/** @return True when one planner result can become the fixed channel-2 envelope. */
[[nodiscard]] bool valid_entity(const Plan& plan, const EntityPayloadPlan& payload) noexcept {
    const auto& allocation = plan.actor.allocation;
    if (plan.peerGeneration == 0 || plan.recordGeneration == 0 || plan.actorSlot >= kActorCapacity
        || allocation.generation == 0 || allocation.actorId == 0 || allocation.sequence < 2
        || allocation.token.index > external::kMaximumEntitySlot
        || allocation.token.incarnation > external::kMaximumEntityIncarnation
        || plan.actor.bubble > 0xFFU || payload.type >= external::EntityType::count) {
        return false;
    }
    if (plan.operation == Operation::create) {
        return plan.fields == kCreateFieldMask;
    }
    if (plan.operation == Operation::update) {
        return plan.fields != 0;
    }
    return plan.operation == Operation::remove && !payload.anchorPresent
           && payload.baseline.byteCount == 0 && payload.update.byteCount == 0;
}

/** Copies one exact planner operation into its generic channel-2 record. */
void fill_entity(const Plan& plan,
                 const EntityPayloadPlan& payload,
                 external::EntityBatch& batch) noexcept {
    batch.recordPresent = true;
    batch.defaultRawBubble = static_cast<std::uint16_t>(plan.actor.bubble);
    external::EntityRecord& record = batch.record;
    record.token = {plan.actor.allocation.token.index, plan.actor.allocation.token.incarnation};
    record.anchor = payload.anchor;
    record.anchorPresent = payload.anchorPresent;
    record.rawBubble = batch.defaultRawBubble;
    record.lifecycleRevision =
        plan.operation == Operation::create ? plan.actor.allocation.sequence : 0;
    record.type = payload.type;
    record.baseline = payload.baseline;
    record.update = payload.update;
    if (plan.operation == Operation::create) {
        record.flags = external::entityCreate | external::entityUpdate;
    } else if (plan.operation == Operation::update) {
        record.flags = external::entityUpdate;
    } else {
        record.flags = external::entityRemove;
    }
    if (payload.anchorPresent) {
        record.flags |= external::entityAnchor;
    }
}

} // namespace

/** Builds one immutable external frame without committing packet outcomes. */
bool build_external_plan(const common::CommonPlan* commonPlan,
                         const Plan* entityPlan,
                         const EntityPayloadPlan* payloadPlan,
                         middleware::gameplay::external::ExternalEntityFrame& frame) noexcept {
    middleware::gameplay::external::ExternalEntityFrame candidate{};
    if ((entityPlan == nullptr) != (payloadPlan == nullptr)) {
        return false;
    }
    if (commonPlan != nullptr) {
        if (!valid_common(*commonPlan)) {
            return false;
        }
        candidate.common = commonPlan->common;
        candidate.commonPresent = true;
    }
    if (entityPlan != nullptr) {
        if (!valid_entity(*entityPlan, *payloadPlan)) {
            return false;
        }
        fill_entity(*entityPlan, *payloadPlan, candidate.entities);
    }
    frame = candidate;
    return true;
}

} // namespace sunrise::server::gameplay::physics::replication
