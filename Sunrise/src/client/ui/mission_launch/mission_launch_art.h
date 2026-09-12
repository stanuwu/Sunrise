#pragma once
#include <cstdint>

#include "../../../state/build_data/activities/activity_artwork.h"
struct ID3D11Device;

namespace sunrise::client::ui::mission_launch::art {
/** Render-lock owned. Each device generation gets one bounded upload attempt per asset. */
void prepare(ID3D11Device* device) noexcept;
/** Releases the render-owner's texture views and device reference before renderer teardown. */
void release() noexcept;
/** @return A render-owned texture handle, or zero when this numeric slot is unavailable. */
[[nodiscard]] std::uint64_t texture(state::build_data::activities::Icon icon) noexcept;
struct DisplaySize {
    float width{}, height{};
};
/** Preserve native aspect ratio; no icon upscales beyond one source pixel per framebuffer pixel. */
[[nodiscard]] DisplaySize display_size(state::build_data::activities::Icon icon,
                                       float extent,
                                       float framebufferScale = 1.0F) noexcept;
} // namespace sunrise::client::ui::mission_launch::art
