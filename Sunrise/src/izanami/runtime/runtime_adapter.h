#pragma once

#include <cstdint>
#include <string_view>

#include "../core/ids.h"
#include "../core/transform.h"
#include "capabilities.h"

namespace sunrise::izanami::runtime {

struct ForgeHandle {
    std::uint64_t value{};

    [[nodiscard]] bool is_valid() const noexcept;

    friend bool operator==(const ForgeHandle&, const ForgeHandle&) noexcept = default;
};

struct BubbleId {
    std::uint32_t value{};
};

struct DestinationId {
    std::uint32_t value{};
};

struct WorldContext {
    BubbleId bubble{};
    DestinationId destination{};
};

enum class ResidencyState : std::uint8_t {
    unknown,
    installed,
    packageAccepted,
    definitionResolved,
    nativeInstance,
};

enum class RuntimeStatus : std::uint8_t {
    ok,
    unsupported,
    unavailable,
    invalidArgument,
    failed,
};

struct ResourceInfo {
    RuntimeStatus status{RuntimeStatus::unsupported};
    core::ResourceId resource{};
    core::ObjectKind kind{core::ObjectKind::forgeOnly};
};

struct SpawnResult {
    RuntimeStatus status{RuntimeStatus::unsupported};
    ForgeHandle handle{};
};

struct TransformResult {
    RuntimeStatus status{RuntimeStatus::unsupported};
    core::Transform transform{};
};

struct Ray {
    core::Vec3 origin{};
    core::Vec3 direction{};
};

struct RaycastResult {
    RuntimeStatus status{RuntimeStatus::unsupported};
    bool hit{};
    float fraction{};
    core::Vec3 position{};
};

class IForgeRuntime {
public:
    virtual ~IForgeRuntime() = default;

    [[nodiscard]] virtual WorldContext world() const noexcept = 0;
    [[nodiscard]] virtual CapabilitySet capabilities() const noexcept = 0;

    [[nodiscard]] virtual ResourceInfo
    inspect_resource(core::ResourceId resource) const noexcept = 0;
    [[nodiscard]] virtual ResidencyState residency(core::ResourceId resource) const noexcept = 0;

    [[nodiscard]] virtual SpawnResult spawn_static(core::ResourceId resource,
                                                   core::Transform transform) noexcept = 0;
    [[nodiscard]] virtual SpawnResult spawn_pattern(core::ResourceId resource,
                                                    core::Transform transform) noexcept = 0;
    [[nodiscard]] virtual RuntimeStatus destroy(ForgeHandle handle) noexcept = 0;

    [[nodiscard]] virtual TransformResult transform(ForgeHandle handle) const noexcept = 0;
    [[nodiscard]] virtual RuntimeStatus set_transform(ForgeHandle handle,
                                                      core::Transform transform) noexcept = 0;

    [[nodiscard]] virtual RaycastResult raycast(Ray ray) const noexcept = 0;
};

[[nodiscard]] IForgeRuntime& unsupported_runtime() noexcept;
[[nodiscard]] std::string_view status_text(RuntimeStatus status) noexcept;
[[nodiscard]] std::string_view residency_text(ResidencyState state) noexcept;

} // namespace sunrise::izanami::runtime
