#include "platform.h"

namespace sunrise::client::hooks::network {

std::atomic<sunrise::client::network::HttpConsumer> g_httpConsumer{};
std::atomic<sunrise::client::network::BapConsumer> g_bapConsumer{};

} // namespace sunrise::client::hooks::network

namespace sunrise::client::network {

/** Registers the single in-process HTTP consumer. */
bool register_http_consumer(HttpConsumer consumer) noexcept {
    if (consumer == nullptr) {
        return false;
    }
    HttpConsumer expected = nullptr;
    return hooks::network::g_httpConsumer.compare_exchange_strong(
        expected, consumer, std::memory_order_release, std::memory_order_relaxed);
}

/** @param consumer Consumer removed only when it owns the registration slot. */
void unregister_http_consumer(HttpConsumer consumer) noexcept {
    HttpConsumer expected = consumer;
    (void)hooks::network::g_httpConsumer.compare_exchange_strong(
        expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

/** Registers the single in-process BAP consumer. */
bool register_bap_consumer(BapConsumer consumer) noexcept {
    if (consumer == nullptr) {
        return false;
    }
    BapConsumer expected = nullptr;
    return hooks::network::g_bapConsumer.compare_exchange_strong(
        expected, consumer, std::memory_order_release, std::memory_order_relaxed);
}

/** @param consumer Consumer removed only when it owns the registration slot. */
void unregister_bap_consumer(BapConsumer consumer) noexcept {
    BapConsumer expected = consumer;
    (void)hooks::network::g_bapConsumer.compare_exchange_strong(
        expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

} // namespace sunrise::client::network
