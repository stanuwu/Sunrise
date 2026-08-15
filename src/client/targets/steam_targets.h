#pragma once

#include <cstddef>
#include <span>

#include "../patterns/registry.h"

namespace sunrise::client::targets::steam {

/** Unowned entry points resolved in Steam networking. */
struct Targets {
    std::byte* authenticationStatus{};
    std::byte* setCertificate{};
};

/** Resolves every required Steam networking entry point uniquely. */
[[nodiscard]] bool resolve(std::span<const patterns::ImageRange> image) noexcept;

/** Clears all resolved Steam networking entry points. */
void clear() noexcept;

/** Returns the process-local Steam target table. */
[[nodiscard]] const Targets& get() noexcept;

} // namespace sunrise::client::targets::steam
