#pragma once

#include <cstdint>
#include <imgui.h>

namespace sunrise::core::ui::animation::transition {

/** Independent animation lanes let one widget blend hover and selection at once. */
enum class Lane : std::uint8_t {
    hover,
    selection,
    focus,
    visibility,
};

/** Fixed keys of the transitions no widget ID names; each visibility-lane user owns one. */
enum class FixedKey : ImGuiID {
    surface = 1,
    startupHint = 2,
    notice = 3,
};

/** @return The widget ID one fixed key stands for. */
[[nodiscard]] constexpr ImGuiID fixed_id(FixedKey key) noexcept {
    return static_cast<ImGuiID>(key);
}

/** Per-second response rates keep transitions stable across frame rates. */
struct Rates {
    float risePerSecond{};
    float fallPerSecond{};
};

/**
 * Advances one bounded transition during the current Dear ImGui frame.
 * The owning render lock must serialize calls.
 * @param id Stable Dear ImGui widget ID.
 * @param lane Independent state lane owned by the widget.
 * @param enabled Picks the normalized target, 1 or 0.
 * @param rates Positive response rates for rising and falling values.
 * @param initialValue Value used when the fixed table first sees this key.
 * @return Current normalized transition value.
 */
[[nodiscard]] float
update(ImGuiID id, Lane lane, bool enabled, Rates rates, float initialValue) noexcept;

/** Clears every transition after all active UI render calls have stopped. */
void reset() noexcept;

} // namespace sunrise::core::ui::animation::transition
