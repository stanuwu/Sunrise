#include "service_registry.h"

#include <algorithm>

namespace sunrise::izanami::kernel {

/** Registers one process-local service by stable ID. */
bool ServiceRegistry::register_service(std::string_view id, void* service) {
    if (id.empty() || service == nullptr) {
        return false;
    }
    const std::lock_guard lock{mutex_};
    const auto found = std::find_if(services_.begin(), services_.end(), [id](const ServiceRecord& record) {
        return record.id == id;
    });
    if (found != services_.end()) {
        return false;
    }
    services_.push_back({.id = id, .service = service});
    return true;
}

/** Removes a registered service by stable ID. */
bool ServiceRegistry::unregister_service(std::string_view id) {
    const std::lock_guard lock{mutex_};
    const auto found = std::find_if(services_.begin(), services_.end(), [id](const ServiceRecord& record) {
        return record.id == id;
    });
    if (found == services_.end()) {
        return false;
    }
    services_.erase(found);
    return true;
}

/** @return Registered service pointer, or null when unavailable. */
void* ServiceRegistry::resolve(std::string_view id) const {
    const std::lock_guard lock{mutex_};
    const auto found = std::find_if(services_.begin(), services_.end(), [id](const ServiceRecord& record) {
        return record.id == id;
    });
    return found == services_.end() ? nullptr : found->service;
}

/** @return Number of registered services. */
std::size_t ServiceRegistry::count() const {
    const std::lock_guard lock{mutex_};
    return services_.size();
}

/** Removes every registered service. */
void ServiceRegistry::clear() {
    const std::lock_guard lock{mutex_};
    services_.clear();
}

} // namespace sunrise::izanami::kernel