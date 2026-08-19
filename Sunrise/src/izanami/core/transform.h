#pragma once

namespace sunrise::izanami::core {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Quat {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

/** Forge-owned transform. Scale is uniform because SMapNodeEntry stores scale in translation.w. */
struct Transform {
    Vec3 translation{};
    Quat rotation{};
    float uniformScale{1.0F};

    [[nodiscard]] bool is_finite() const noexcept;
};

} // namespace sunrise::izanami::core