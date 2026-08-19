#pragma once

#include <cstdint>

#include "../../internal.h"

namespace sunrise::server::bap::encrypted::transactions {

/** Connection fields published only after State commits and caller output is copied. */
struct Publication {
    ActivityClientBinding activity{};
    bool hasActivitySessionBinding{};
    /** Rejoin keeps the exact retained private/public owner and only resets connection state. */
    bool preservesActivitySessionBinding{};
};

} // namespace sunrise::server::bap::encrypted::transactions
