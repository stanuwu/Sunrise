#pragma once

#include <cstdint>

#include "../internal.h"

namespace sunrise::middleware::signon::extended {

/**
 * Appends the optional SignOn success fields: the extended blob and the three config URLs.
 * The Client accepts all four. None has a proven reader, so the values are fixed, not derived.
 * @param success Writer bound to the success sub-message storage.
 * @param relayAddress Relay address, byte order matching the required relay field.
 * @return True when every optional field fits.
 */
[[nodiscard]] bool append(Writer& success, std::uint32_t relayAddress) noexcept;

} // namespace sunrise::middleware::signon::extended
