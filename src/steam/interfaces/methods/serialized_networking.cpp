#include "../internal.h"

namespace sunrise::steam::interfaces::methods {

/** Drops a rendezvous notification. Nothing is sent. */
void serialized_send_rendezvous([[maybe_unused]] void* self,
                                [[maybe_unused]] std::uint64_t remoteId,
                                [[maybe_unused]] DWORD sourceConnectionId,
                                [[maybe_unused]] const void* message,
                                [[maybe_unused]] DWORD messageSize) noexcept {}

/** Drops a connection failure message. Nothing is sent. */
void serialized_send_failure([[maybe_unused]] void* self,
                             [[maybe_unused]] std::uint64_t remoteId,
                             [[maybe_unused]] DWORD destinationConnectionId,
                             [[maybe_unused]] DWORD reason,
                             [[maybe_unused]] const char* reasonText) noexcept {}

/** @return Zero. This shim delivers no serialized certificates. */
ApiCall serialized_get_certificate([[maybe_unused]] void* self) noexcept {
    return 0;
}

/**
 * Writes an empty network config.
 * @param output Optional text buffer.
 * @return Zero config bytes.
 */
int serialized_get_network_config([[maybe_unused]] void* self,
                                  void* output,
                                  DWORD capacity,
                                  [[maybe_unused]] const char* launcherPartner) noexcept {
    if (output != nullptr && capacity > 0) {
        *static_cast<char*>(output) = '\0';
    }
    return 0;
}

/** Drops a relay ticket. Nothing is kept. */
void serialized_cache_relay_ticket([[maybe_unused]] void* self,
                                   [[maybe_unused]] const void* ticket,
                                   [[maybe_unused]] DWORD ticketSize) noexcept {}

/** @return Zero. No relay tickets are kept. */
DWORD serialized_relay_ticket_count([[maybe_unused]] void* self) noexcept {
    return 0;
}

/** @return Zero. There is no ticket at any index. */
int serialized_get_relay_ticket([[maybe_unused]] void* self,
                                [[maybe_unused]] DWORD index,
                                [[maybe_unused]] void* output,
                                [[maybe_unused]] DWORD capacity) noexcept {
    return 0;
}

/** Drops a connection-state message. Nothing is kept. */
void serialized_post_connection_state([[maybe_unused]] void* self,
                                      [[maybe_unused]] const void* message,
                                      [[maybe_unused]] DWORD messageSize) noexcept {}

} // namespace sunrise::steam::interfaces::methods
