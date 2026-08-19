#include "runtime_adapter.h"

namespace sunrise::izanami::runtime {

namespace {

class UnsupportedRuntime final : public IForgeRuntime {
public:
    [[nodiscard]] WorldContext world() const noexcept override {
        return {};
    }

    [[nodiscard]] CapabilitySet capabilities() const noexcept override {
        return {};
    }

    [[nodiscard]] ResourceInfo inspect_resource(core::ResourceId resource) const noexcept override {
        return {.resource = resource};
    }

    [[nodiscard]] ResidencyState residency(core::ResourceId) const noexcept override {
        return ResidencyState::unknown;
    }

    [[nodiscard]] SpawnResult spawn_static(core::ResourceId, core::Transform) noexcept override {
        return {};
    }

    [[nodiscard]] SpawnResult spawn_pattern(core::ResourceId, core::Transform) noexcept override {
        return {};
    }

    [[nodiscard]] RuntimeStatus destroy(ForgeHandle) noexcept override {
        return RuntimeStatus::unsupported;
    }

    [[nodiscard]] TransformResult transform(ForgeHandle) const noexcept override {
        return {};
    }

    [[nodiscard]] RuntimeStatus set_transform(ForgeHandle, core::Transform) noexcept override {
        return RuntimeStatus::unsupported;
    }

    [[nodiscard]] RaycastResult raycast(Ray) const noexcept override {
        return {};
    }
};

UnsupportedRuntime g_runtime;

} // namespace

/** @return A fail-closed adapter used until native Sunrise capabilities are validated. */
IForgeRuntime& unsupported_runtime() noexcept {
    return g_runtime;
}

/** @return True when the handle names a live runtime binding. */
bool ForgeHandle::is_valid() const noexcept {
    return value != 0;
}

/** @return Stable diagnostic text for one runtime call result. */
std::string_view status_text(RuntimeStatus status) noexcept {
    switch (status) {
    case RuntimeStatus::ok:
        return "ok";
    case RuntimeStatus::unsupported:
        return "unsupported";
    case RuntimeStatus::unavailable:
        return "unavailable";
    case RuntimeStatus::invalidArgument:
        return "invalid_argument";
    case RuntimeStatus::failed:
        return "failed";
    }
    return "unknown";
}

/** @return Stable diagnostic text for one resource residency state. */
std::string_view residency_text(ResidencyState state) noexcept {
    switch (state) {
    case ResidencyState::unknown:
        return "unknown";
    case ResidencyState::installed:
        return "installed";
    case ResidencyState::packageAccepted:
        return "package_accepted";
    case ResidencyState::definitionResolved:
        return "definition_resolved";
    case ResidencyState::nativeInstance:
        return "native_instance";
    }
    return "unknown";
}

} // namespace sunrise::izanami::runtime
