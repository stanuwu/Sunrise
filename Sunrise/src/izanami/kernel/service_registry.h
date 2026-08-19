#pragma once

#include <cstddef>
#include <mutex>
#include <string_view>
#include <vector>

namespace sunrise::izanami::kernel {

struct ServiceRecord {
    std::string_view id{};
    void* service{};
};

class ServiceRegistry final {
public:
    [[nodiscard]] bool register_service(std::string_view id, void* service);
    [[nodiscard]] bool unregister_service(std::string_view id);
    [[nodiscard]] void* resolve(std::string_view id) const;
    [[nodiscard]] std::size_t count() const;
    void clear();

private:
    mutable std::mutex mutex_{};
    std::vector<ServiceRecord> services_{};
};

} // namespace sunrise::izanami::kernel