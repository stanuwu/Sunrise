#pragma once

#include "../definition.h"

namespace sunrise::server::bap::encrypted::queuez::staging {

/** @return True for a logical match between two resident rows. */
[[nodiscard]] bool same_resident(const ResidentObject& left, const ResidentObject& right) noexcept;

/**
 * Compares two canonical peer states field by field. A pending plan is rechecked this way against
 * the before-image it was staged from.
 * @return True when both are valid and every fixed Family-4 field matches.
 */
[[nodiscard]] bool same_state(const SessionState& left, const SessionState& right) noexcept;

} // namespace sunrise::server::bap::encrypted::queuez::staging
