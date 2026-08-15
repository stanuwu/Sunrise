#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::client::network {

/** 256 KiB caps the fixed storage. The Client's one-way BAP frames are large. */
inline constexpr std::size_t kBapFrameCapacity = 256 * 1024;

/**
 * Fixed BAP connection slots shared by the transport and the Server.
 * The activity route opens a second link beside the primary one, so one slot is never enough.
 */
inline constexpr std::size_t kBapConnectionCount = 4;

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
};

/** BAP completion fields written by the Server consumer. */
struct BapResponse {
    std::size_t size{};
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
