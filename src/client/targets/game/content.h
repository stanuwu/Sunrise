#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../patterns/registry.h"

namespace sunrise::client::targets::game::content {

/** Unowned main-image entry points required only by runtime content extraction. */
struct Targets {
    std::byte* queuezObjectStoreGetter{};
    std::byte* contentHandleTablesSlot{};
    std::byte* getItemStatValue{};
    std::byte* lightValueToScalar{};
    std::uint32_t queuezDescriptorFamilyBias{};
};

/** Resolves and publishes the complete late content target group. */
[[nodiscard]] bool resolve(std::span<const patterns::ImageRange> image) noexcept;

/** Clears the published late content target group. */
void clear() noexcept;

/** @return Process-local late content target group. */
[[nodiscard]] const Targets& get() noexcept;

/** @return True after the complete late content group is published. */
[[nodiscard]] bool is_resolved() noexcept;

} // namespace sunrise::client::targets::game::content
