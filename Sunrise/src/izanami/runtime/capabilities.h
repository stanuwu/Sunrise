#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::izanami::runtime {

enum class Capability : std::uint8_t {
    worldStaticSpawn,
    worldPatternSpawn,
    worldEntitySpawnLoaded,
    worldDestroy,
    worldTransformRead,
    worldTransformWrite,
    physicsRaycastPosition,
    physicsRaycastObject,
    resourceInspect,
    resourceResidency,
    count,
};

class CapabilitySet final {
public:
    [[nodiscard]] bool has(Capability capability) const noexcept;
    void set(Capability capability, bool enabled) noexcept;
    void clear() noexcept;

private:
    static constexpr std::size_t kCount = static_cast<std::size_t>(Capability::count);

    std::array<bool, kCount> enabled_{};
};

} // namespace sunrise::izanami::runtime