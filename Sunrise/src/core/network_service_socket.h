#pragma once

#include <cstdint>

namespace sunrise::core::network {

/**
 * Keeps a shared service's explicit reply address intact through the game's Winsock hooks.
 * The active socket is thread-local: each thread sees only the scope it entered itself.
 */
class ServiceSocketScope {
public:
    /** Temporarily replaces this thread's service socket; nested scopes restore their parent. */
    explicit ServiceSocketScope(std::uintptr_t socket) noexcept : previous_(active_) {
        active_ = socket;
    }
    /** Restores the socket owned by the enclosing scope, or the no-socket sentinel. */
    ~ServiceSocketScope() noexcept {
        active_ = previous_;
    }
    ServiceSocketScope(const ServiceSocketScope&) = delete;
    ServiceSocketScope& operator=(const ServiceSocketScope&) = delete;
    /** @return True while this thread is inside a scope that owns `socket`. */
    [[nodiscard]] static bool owns(std::uintptr_t socket) noexcept {
        return socket != invalid_ && socket == active_;
    }

private:
    static constexpr std::uintptr_t invalid_ = ~std::uintptr_t{};
    static inline thread_local std::uintptr_t active_ = invalid_;
    std::uintptr_t previous_;
};

} // namespace sunrise::core::network
