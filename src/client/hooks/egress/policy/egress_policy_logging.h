#pragma once

#include "policy.h"

namespace sunrise::client::hooks::egress::policy {

/**
 * Logs one fixed debug event, with no endpoint or payload data.
 * @param operation Socket operation category.
 * @param targetsRedirect True when the checked target is the exact redirect target.
 * @param allowed True when the original can be called.
 */
void log_decision(SocketOperation operation, bool targetsRedirect, bool allowed) noexcept;

} // namespace sunrise::client::hooks::egress::policy
