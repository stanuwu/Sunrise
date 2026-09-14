#pragma once

#include <cstdint>

namespace sunrise::core::network {

/** Keeps a shared service's explicit reply address intact through the game's Winsock hooks. */
class ServiceSocketScope {
public:
    explicit ServiceSocketScope(std::uintptr_t socket) noexcept : previous_(active_) {
        active_ = socket;
    }
    ~ServiceSocketScope() noexcept {
        active_ = previous_;
    }
    ServiceSocketScope(const ServiceSocketScope&) = delete;
    ServiceSocketScope& operator=(const ServiceSocketScope&) = delete;
    [[nodiscard]] static bool owns(std::uintptr_t socket) noexcept {
        return socket != invalid_ && socket == active_;
    }

private:
    static constexpr std::uintptr_t invalid_ = ~std::uintptr_t{};
    static inline thread_local std::uintptr_t active_ = invalid_;
    std::uintptr_t previous_;
};

} // namespace sunrise::core::network
