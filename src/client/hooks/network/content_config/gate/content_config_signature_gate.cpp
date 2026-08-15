#include "content_config_signature_gate.h"

#include <atomic>

namespace sunrise::client::hooks::network::content_config::gate {
namespace {

/** The signed-manifest check is skipped only while the gate byte is zero. */
constexpr char kDisabledValue = 0;
static_assert(std::atomic<char>::is_always_lock_free);

/** @param address Writable gate byte. @return The current byte, read atomically. */
[[nodiscard]] char read(std::byte* address) noexcept {
    return std::atomic_ref(*reinterpret_cast<char*>(address)).load(std::memory_order_acquire);
}

} // namespace

/** Turns off a checked, writable signature gate with a compare-exchange. */
bool disable(std::byte* address, Ownership& ownership) noexcept {
    ownership = {};
    if (address == nullptr) {
        return false;
    }
    const char previous = read(address);
    ownership.address = address;
    if (previous == kDisabledValue) {
        return true;
    }
    char expected = previous;
    if (!std::atomic_ref(*reinterpret_cast<char*>(address))
             .compare_exchange_strong(
                 expected, kDisabledValue, std::memory_order_acq_rel, std::memory_order_acquire)) {
        ownership = {};
        return false;
    }
    ownership.previous = previous;
    ownership.owned = true;
    return true;
}

/** Rewrites the zero when the Client's own content init has restored the gate. */
bool reassert(Ownership& ownership) noexcept {
    if (ownership.address == nullptr) {
        return false;
    }
    const char previous = read(ownership.address);
    if (previous == kDisabledValue) {
        return true;
    }
    char expected = previous;
    if (!std::atomic_ref(*reinterpret_cast<char*>(ownership.address))
             .compare_exchange_strong(
                 expected, kDisabledValue, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    // Keep the first value we replaced, so detach puts back what the Client last set.
    if (!ownership.owned) {
        ownership.previous = previous;
        ownership.owned = true;
    }
    return true;
}

/** Puts the old byte back, but only if this lifecycle wrote the zero. */
bool restore(Ownership& ownership) noexcept {
    if (ownership.address == nullptr || !ownership.owned) {
        ownership = {};
        return true;
    }
    char expected = kDisabledValue;
    (void)std::atomic_ref(*reinterpret_cast<char*>(ownership.address))
        .compare_exchange_strong(
            expected, ownership.previous, std::memory_order_acq_rel, std::memory_order_acquire);
    ownership = {};
    return true;
}

/** @param ownership Gate ownership state. @return True when the gate is currently zero. */
bool is_disabled(const Ownership& ownership) noexcept {
    return ownership.address != nullptr && read(ownership.address) == kDisabledValue;
}

} // namespace sunrise::client::hooks::network::content_config::gate
