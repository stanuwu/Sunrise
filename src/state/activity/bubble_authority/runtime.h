#pragma once

#include <cstdint>

#include "../definition.h"

namespace sunrise::state::activity::bubble_authority {

/**
 * Picks the bubble to hand this session, if one is owed.
 * A bubble is granted once. The token is a change against the client's own mirror, so re-sending
 * the same token for a bubble already granted does nothing, rather than being an error.
 * @param sessionId Joined activity session.
 * @param sliceSetIndex Slice set the client is in, or the destination's own.
 * @param grant Gets the bubble and its token.
 * @return True when a bubble is owed.
 */
[[nodiscard]] bool
select_grant(std::uint64_t sessionId, std::int32_t sliceSetIndex, Grant& grant) noexcept;

/**
 * Records a bubble as granted so it is not granted twice.
 * @param sessionId Joined activity session.
 * @param grant Bubble and token that went out.
 */
void record_grant(std::uint64_t sessionId, const Grant& grant) noexcept;

} // namespace sunrise::state::activity::bubble_authority
