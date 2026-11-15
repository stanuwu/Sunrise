#pragma once

#include <cstdint>

#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../state/activity_sdk/runtime.h"
#include "activity_sdk_device_runtime.h"

namespace sunrise::server::activity::host {
struct ScriptableOutputReservation;
}

namespace sunrise::server::activity::activity_sdk_lifetime {

/** Shared SDK adapter refusal surface; `invalidValue` is an out-of-range lifetime state. */
using Status = activity_sdk_devices::Status;

/** Checks one activity lifetime change without changing Activity Host output state. */
[[nodiscard]] Status availability(const state::activity_sdk::BoundView& view,
                                  std::uint8_t lifetimeState) noexcept;

/** Sets one activity lifetime state for an operator action. */
[[nodiscard]] Status set(const state::activity_sdk::BoundView& view,
                         std::uint8_t lifetimeState) noexcept;

/** Queues one lifetime change only through the exact unarmed Host revision owned by Mission. */
[[nodiscard]] Status set_reserved(const state::activity_sdk::BoundView& view,
                                  std::uint8_t lifetimeState,
                                  const host::ScriptableOutputReservation& reservation) noexcept;

/** Queues one activity flag change; a full flag list refuses a new raised flag. */
[[nodiscard]] Status set_activity_flag_reserved(
    const state::activity_sdk::BoundView& view,
    const middleware::bap::activity_message::sensor_auth_update::ActivityFlagChange& change,
    const host::ScriptableOutputReservation& reservation) noexcept;

} // namespace sunrise::server::activity::activity_sdk_lifetime
