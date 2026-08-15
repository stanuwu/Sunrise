#pragma once

#include <cstdint>

namespace sunrise::server::bap::encrypted::transactions {

/** Connection fields published only after State commits and caller output is copied. */
struct Publication {
    std::uint64_t activitySessionId{};
    bool hasActivitySessionBinding{};
};

} // namespace sunrise::server::bap::encrypted::transactions
