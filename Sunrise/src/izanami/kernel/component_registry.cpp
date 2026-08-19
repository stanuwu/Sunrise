#include "component_registry.h"

#include <algorithm>

namespace sunrise::izanami::kernel {

/** Registers one Forge-owned component type. */
bool ComponentRegistry::register_component(ComponentDescriptor descriptor) {
    if (descriptor.typeId == 0 || descriptor.name.empty()) {
        return false;
    }
    const std::lock_guard lock{mutex_};
    const auto found = std::find_if(
        components_.begin(), components_.end(), [descriptor](const ComponentDescriptor& existing) {
            return existing.typeId == descriptor.typeId;
        });
    if (found != components_.end()) {
        return false;
    }
    components_.push_back(descriptor);
    return true;
}

/** @return Registered component descriptor, or null when absent. */
const ComponentDescriptor* ComponentRegistry::find(std::uint32_t typeId) const {
    const std::lock_guard lock{mutex_};
    const auto found = std::find_if(components_.begin(), components_.end(), [typeId](const ComponentDescriptor& descriptor) {
        return descriptor.typeId == typeId;
    });
    return found == components_.end() ? nullptr : &*found;
}

/** @return Number of registered component types. */
std::size_t ComponentRegistry::count() const {
    const std::lock_guard lock{mutex_};
    return components_.size();
}

/** Clears every registered component type. */
void ComponentRegistry::clear() {
    const std::lock_guard lock{mutex_};
    components_.clear();
}

} // namespace sunrise::izanami::kernel