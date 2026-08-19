#include "capabilities.h"

namespace sunrise::izanami::runtime {

namespace {

[[nodiscard]] std::size_t index_of(Capability capability) noexcept {
    return static_cast<std::size_t>(capability);
}

[[nodiscard]] bool is_valid(Capability capability) noexcept {
    return index_of(capability) < static_cast<std::size_t>(Capability::count);
}

} // namespace

/** @return True when the runtime advertises this native/editor capability. */
bool CapabilitySet::has(Capability capability) const noexcept {
    return is_valid(capability) && enabled_[index_of(capability)];
}

/** Enables or disables one capability. Invalid values are ignored so callers fail closed. */
void CapabilitySet::set(Capability capability, bool enabled) noexcept {
    if (is_valid(capability)) {
        enabled_[index_of(capability)] = enabled;
    }
}

/** Disables every capability. */
void CapabilitySet::clear() noexcept {
    enabled_ = {};
}

} // namespace sunrise::izanami::runtime