#pragma once

#include <cstddef>
#include <string_view>

namespace sunrise::core::ui::notice {

/** Longest notice text kept. It does not include a null. */
inline constexpr std::size_t kTextCapacity = 128;

/**
 * Raises one user-visible notice about a Sunrise failure. Never blocks, so a fault handler can
 * call it. A notice is dropped, not waited on, when the storage is busy.
 * @param text Message. Longer text is cut.
 */
void raise(std::string_view text) noexcept;

/**
 * Draws waiting notices inside the caller's active Dear ImGui frame. It runs even when the
 * interface is hidden, so a failure reaches the user without the log.
 * @return True when draw data was built for them.
 */
[[nodiscard]] bool draw() noexcept;

} // namespace sunrise::core::ui::notice
