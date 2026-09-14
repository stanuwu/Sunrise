#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../core/network_capacity.h"

namespace sunrise::client::network {

/** The largest svc8 frame is a 0x7D800-byte activity payload plus 49 wire bytes. */
inline constexpr std::size_t kBapFrameCapacity = 0x7D800 + 49;

/**
 * Fixed BAP connection slots shared by the transport and the Server.
 * A crossing holds the old and the new activity link at once, so a lobby plus three activity
 * links is reachable. Below that the accept is refused and the connect stalls.
 * The shared
 * listener allows these overlaps independently for several real players.
 */
inline constexpr std::size_t kBapConnectionCount = core::network_capacity::kConnections;

/** HTTP request view passed from Client hooks to Server. Every span carries its size. */
struct HttpRequest {
    std::string_view url;
    std::string_view contentType;
    std::span<const std::byte> body;
    std::span<std::byte> response;
};

/** HTTP completion fields written by the Server consumer. */
struct HttpResponse {
    std::size_t size{};
    unsigned statusCode{};
};

/** Lifecycle event raised by the BAP transport. */
enum class BapEvent : std::uint8_t {
    open,
    frame,
    /** Timed service event, with no inbound bytes. */
    poll,
    close,
};

/** Connection-scoped BAP exchange with caller-owned frame storage. */
struct BapRequest {
    BapEvent event{};
    std::uint32_t connectionId{};
    std::span<const std::byte> frame{};
    std::span<std::byte> response{};
    /** Host-order IPv4 source, captured by accept rather than a client frame. */
    std::uint32_t remoteAddress{};
};

/** BAP completion fields written by the Server consumer. */
struct BapResponse {
    std::size_t size{};
    /** The transport closes after the consumer has released its session lock. */
    bool closeConnection{};
    /** The complete inbound frame stays buffered until ordered reply capacity is available. */
    bool deferFrame{};
    /** The owning session completed its hello; transport timeouts never infer this from bytes. */
    bool authenticated{};
};

/** In-process HTTP route with caller-owned request and response storage. */
using HttpConsumer = bool (*)(const HttpRequest&, HttpResponse&) noexcept;

/** In-process BAP route with explicit connection lifecycle events. */
using BapConsumer = bool (*)(const BapRequest&, BapResponse&) noexcept;

/** Registers the Server consumer used by the HTTP detour. */
[[nodiscard]] bool register_http_consumer(HttpConsumer consumer) noexcept;

/** Removes the Server consumer when it still matches. */
void unregister_http_consumer(HttpConsumer consumer) noexcept;

/** Registers the Server BAP consumer. */
[[nodiscard]] bool register_bap_consumer(BapConsumer consumer) noexcept;

/** Removes the Server BAP consumer when it still matches. */
void unregister_bap_consumer(BapConsumer consumer) noexcept;

} // namespace sunrise::client::network
