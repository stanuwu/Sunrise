#include "activity_sdk_lifetime_runtime.h"

#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../bap/runtime.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_lifetime {
namespace {

namespace sdk = state::activity_sdk;

/** Preflights one lifetime change and keeps the link fields the queue call needs. */
[[nodiscard]] Status prepare(const sdk::BoundView& view,
                             std::uint8_t lifetimeState,
                             server::bap::ActivityLinkView& link) noexcept {
    link = {};
    // Above the highest jump-table entry the client's spawn gate jumps out of its image.
    if (lifetimeState
        > middleware::bap::activity_message::sensor_auth_update::kMaximumLifetimeState) {
        return Status::invalidValue;
    }
    return activity_sdk_devices::live_binding_status(view, link);
}

} // namespace

Status availability(const sdk::BoundView& view, std::uint8_t lifetimeState) noexcept {
    server::bap::ActivityLinkView link{};
    return prepare(view, lifetimeState, link);
}

/** Sets the lifetime state for an operator action, which owns no Mission revision. */
Status set(const sdk::BoundView& view, std::uint8_t lifetimeState) noexcept {
    server::bap::ActivityLinkView link{};
    const Status status = prepare(view, lifetimeState, link);
    if (status != Status::ready) {
        return status;
    }
    if (server::bap::request_activity_lifetime_override(
            view.binding, lifetimeState, link.effectiveRegion, link.activityClientGeneration)) {
        return Status::queued;
    }
    host::InstanceSnapshot instance{};
    const bool busy = host::instance_snapshot(view.binding, instance) && instance.outputPending;
    return busy ? Status::outputBusy : Status::refused;
}

/**
 * Queues one lifetime change through an already owned Host revision.
 * @param view Pinned SDK view naming the exact activity generation.
 * @param lifetimeState Activity lifetime state to report.
 * @param reservation Unarmed Host revision the durable Mission head already owns.
 * @return queued on success, or the refusal that stopped it.
 */
Status set_reserved(const sdk::BoundView& view,
                    std::uint8_t lifetimeState,
                    const host::ScriptableOutputReservation& reservation) noexcept {
    server::bap::ActivityLinkView link{};
    const Status status = prepare(view, lifetimeState, link);
    if (status != Status::ready) {
        return status;
    }
    const bool queued =
        server::bap::request_activity_lifetime_override(view.binding,
                                                        lifetimeState,
                                                        link.effectiveRegion,
                                                        link.activityClientGeneration,
                                                        &reservation);
    if (queued) {
        return Status::queued;
    }
    host::InstanceSnapshot instance{};
    const bool busy = host::instance_snapshot(view.binding, instance) && instance.outputPending;
    return busy ? Status::outputBusy : Status::refused;
}

/**
 * Queues one activity flag change through an already owned Host revision.
 * @return queued on success; invalidValue when the flag list has no room for a new raised flag.
 */
Status set_activity_flag_reserved(
    const sdk::BoundView& view,
    const middleware::bap::activity_message::sensor_auth_update::ActivityFlagChange& change,
    const host::ScriptableOutputReservation& reservation) noexcept {
    server::bap::ActivityLinkView link{};
    const Status status = activity_sdk_devices::live_binding_status(view, link);
    if (status != Status::ready) {
        return status;
    }
    if (server::bap::request_activity_flag_override(view.binding,
                                                    change,
                                                    link.effectiveRegion,
                                                    link.activityClientGeneration,
                                                    &reservation)) {
        return Status::queued;
    }
    host::InstanceSnapshot instance{};
    if (!host::instance_snapshot(view.binding, instance)) {
        return Status::refused;
    }
    if (instance.outputPending) {
        return Status::outputBusy;
    }
    return middleware::bap::activity_message::sensor_auth_update::apply_activity_flag(
               instance.activityFlags, change)
               ? Status::refused
               : Status::invalidValue;
}

} // namespace sunrise::server::activity::activity_sdk_lifetime
