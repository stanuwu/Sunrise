#pragma once

#include <cstddef>
#include <span>

#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Writes the periodic activity-link keepalive when one is due. The link tears down after about
 * 20 s of server silence. Membership publishes only once the client has sent its identity, so
 * this tick delivers message 12 and clears `timed_out_waiting_to_establish_client`.
 * @param session Connection-owned nonce, activity binding, and keepalive timer.
 * @param scratch Lock-owned transform buffers.
 * @param response Caller-owned complete-frame storage.
 * @param written Receives encoded notification bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when at least one complete notification is published.
 */
[[nodiscard]] bool consume_activity_keepalive(Session& session,
                                              Scratch& scratch,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              bool& touchesScratch) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
