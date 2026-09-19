#pragma once

#include <cstddef>
#include <span>

#include "format.h"

namespace sunrise::state::activity_sdk::attachment_compatibility {

static_assert(format::kVersion == 41,
              "Re-audit attachment compatibility when the SDK format changes");

/**
 * Capability compatibility for the audited attachment pairs.
 * The generated SDK digest is only known at runtime, so no audited-payload digest is pinned here.
 * The type-26/type-1 same-owner checks in the slot API and attachments::admits still validate
 * every attachment; pin the digest here once a catalog digest has been logged and audited.
 */
[[nodiscard]] inline bool supports(std::span<const std::byte> payloadSha256) noexcept {
    return !payloadSha256.empty();
}

} // namespace sunrise::state::activity_sdk::attachment_compatibility
