#pragma once

#include "../runtime.h"

namespace sunrise::state::activity::bubble_authority::transactions {

/** @return True when two grant entries carry the same typed values. */
inline bool equal(const Grant& first, const Grant& second) noexcept {
    return first.bubble == second.bubble && first.token == second.token;
}

/** @return True when two persistent token tables are identical. */
inline bool equal(const AuthorityState& first, const AuthorityState& second) noexcept {
    return first.grantTokens == second.grantTokens;
}

} // namespace sunrise::state::activity::bubble_authority::transactions
