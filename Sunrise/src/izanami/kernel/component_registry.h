#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace sunrise::izanami::kernel {

struct ComponentDescriptor {
    std::uint32_t typeId{};
    std::uint32_t schemaVersion{};
    std::string_view name{};
};

class ComponentRegistry final {
public:
    [[nodiscard]] bool register_component(ComponentDescriptor descriptor);
    [[nodiscard]] const ComponentDescriptor* find(std::uint32_t typeId) const;
    [[nodiscard]] std::size_t count() const;
    void clear();

private:
    mutable std::mutex mutex_{};
    std::vector<ComponentDescriptor> components_{};
};

} // namespace sunrise::izanami::kernel