#pragma once

#include <cstddef>
#include <span>

#include "../../../../state/activity/definition.h"

namespace sunrise::server::bap::encrypted::activity_host_manager {

/**
 * Prepares one runtime activity session and encodes its svc-7 response body.
 * The request view is borrowed only for validation. State gets typed field-1 scalars only.
 * @param requestBody Whole decrypted svc-6 body.
 * @param output Caller-owned response-body storage.
 * @param written Receives the exact encoded body size.
 * @param allocation Receives a deferred State allocation consumed after frame staging.
 * @param hasAllocation Receives true only when allocation must be committed.
 * @return True when the request, State preparation, and response encoding all succeed.
 */
[[nodiscard]] bool encode_response(std::span<const std::byte> requestBody,
                                   std::span<std::byte> output,
                                   std::size_t& written,
                                   state::activity::PendingAllocation& allocation,
                                   bool& hasAllocation) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_host_manager
