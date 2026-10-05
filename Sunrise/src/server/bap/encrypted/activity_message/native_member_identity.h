#pragma once

#include "../../../../middleware/bap/activity_message/telemetry.h"
#include "../../../../state/activity/membership/definition.h"

namespace sunrise::server::bap::encrypted::activity_message {
/** Converts the native BC byte-array keys to the existing message-23 State representation. */
[[nodiscard]] inline state::activity::membership::Identity native_member_identity(
    const middleware::bap::activity_message::telemetry::ReservationRecord& source) noexcept {
    state::activity::membership::Identity identity{source.machineKey,
                                                   source.memberIndex,
                                                   source.field2,
                                                   0,
                                                   static_cast<std::uint64_t>(source.accountSoid),
                                                   static_cast<std::uint64_t>(source.characterSoid),
                                                   source.groupMemberQword};
    for (unsigned byte = 0; byte < 8; ++byte) {
        identity.joinIdentity =
            (identity.joinIdentity << 8U) | ((source.playerKey >> (8U * byte)) & 255U);
    }
    return identity;
}
} // namespace sunrise::server::bap::encrypted::activity_message
