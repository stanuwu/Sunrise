#pragma once

#include <cstdint>

namespace sunrise::middleware::queuez {

/** Client-picked replicated family id shared by subscription transports. */
struct Subscription {
    std::uint32_t familyType{};
    std::uint64_t familyRootSoid{};
};

} // namespace sunrise::middleware::queuez
