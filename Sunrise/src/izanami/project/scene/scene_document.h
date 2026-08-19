#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "forge_object.h"

namespace sunrise::izanami::project::scene {

class SceneDocument final {
public:
    void clear();

    [[nodiscard]] bool create(ForgeObject object);
    [[nodiscard]] bool erase(core::ForgeUUID id);
    [[nodiscard]] bool set_transform(core::ForgeUUID id, core::Transform transform);
    [[nodiscard]] bool reparent(core::ForgeUUID id, core::ForgeUUID parent);

    [[nodiscard]] ForgeObject* find(core::ForgeUUID id) noexcept;
    [[nodiscard]] const ForgeObject* find(core::ForgeUUID id) const noexcept;
    [[nodiscard]] bool contains(core::ForgeUUID id) const noexcept;
    [[nodiscard]] std::span<const ForgeObject> objects() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;

private:
    std::vector<ForgeObject> objects_{};
};

} // namespace sunrise::izanami::project::scene