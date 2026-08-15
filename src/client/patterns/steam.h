#pragma once

#include <cstddef>
#include <span>

#include "registry.h"

namespace sunrise::client::patterns::steam {

/** Stable indices for Steam networking signatures. */
enum class Id : std::size_t {
    authenticationStatus,
    setCertificate,
    count,
};

/** Returns the immutable Steam networking signature table. */
[[nodiscard]] std::span<const patterns::Pattern> definitions() noexcept;

} // namespace sunrise::client::patterns::steam
