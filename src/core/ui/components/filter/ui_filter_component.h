#pragma once

#include <cstddef>
#include <imgui.h>

namespace sunrise::core::ui::components::filter {

/**
 * Draws an animated text filter and a clear action without owning text storage.
 * @param id Stable Dear ImGui scope ID.
 * @param hint Hint shown while the buffer is empty.
 * @param buffer Caller-owned null-terminated text storage.
 * @param capacity Writable buffer size in bytes.
 * @param flags Dear ImGui input flags that need no callback.
 * @return True when input or the clear action changed the buffer.
 */
[[nodiscard]] bool input(const char* id,
                         const char* hint,
                         char* buffer,
                         std::size_t capacity,
                         ImGuiInputTextFlags flags = ImGuiInputTextFlags_None) noexcept;

} // namespace sunrise::core::ui::components::filter
