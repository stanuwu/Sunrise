#pragma once

#include "definition.h"

namespace sunrise::server::bap::encrypted {

struct ServiceOutcome;

namespace transactions {

/**
 * Commits at most one delayed State transaction.
 * @param outcome Checked service result whose pending transaction is used up.
 * @param publication Gets connection fields to publish after the output copy.
 * @return True when there is no mutation, or the one mutation commits.
 */
[[nodiscard]] bool commit(ServiceOutcome& outcome, Publication& publication) noexcept;

} // namespace transactions

} // namespace sunrise::server::bap::encrypted
