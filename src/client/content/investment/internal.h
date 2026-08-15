#pragma once

#include "../../../state/account/account_state.h"
#include "source.h"

namespace sunrise::client::content::investment {

/** @return True when the next refresh slice must hold the process for a package sweep. */
[[nodiscard]] bool requires_process_freeze() noexcept;

} // namespace sunrise::client::content::investment
