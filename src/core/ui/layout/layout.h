#pragma once

#include <array>
#include <cstddef>

#include "../modules/ui_module_descriptor.h"

namespace sunrise::core::ui::layout {

/** Copy of the selection state. No renderer storage is exposed. */
struct StateSnapshot {
    bool initialized{};
    std::array<char, modules::kStableIdCapacity> selectedStableId{};
    std::size_t selectedStableIdLength{};
};

/**
 * Installs the fixed memory, runtime fonts, and the one Core-owned Dear ImGui context.
 * @return True when the layout is initialized or was already active.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Draws the centered Sunrise surface inside the caller's active Dear ImGui frame.
 * The surface opens and closes over a short transition, so a hidden frame still draws until it
 * has finished closing.
 * @param visible Current Core visibility state, which is the transition target.
 * @return True when draw data was built and the caller must submit the frame.
 */
[[nodiscard]] bool render(bool visible) noexcept;

/**
 * Releases runtime fonts and destroys the context after both presentation backends stop.
 * @return False when a live Dear ImGui allocation delays restoring the allocator.
 */
[[nodiscard]] bool shutdown() noexcept;

/** @return One copy of the init and module selection state, read under the lock. */
[[nodiscard]] StateSnapshot snapshot() noexcept;

} // namespace sunrise::core::ui::layout
