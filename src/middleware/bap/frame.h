#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap {

/** Supported BAP outer-frame encodings. */
enum class FrameType : std::uint8_t {
    /** Plaintext outer marker used by client requests. */
    plaintext0 = 0,
    /** Authenticated AES-GCM outer marker used after bootstrap. */
    encrypted = 1,
    /** Plaintext outer marker used by bootstrap request and response frames. */
    plaintext2 = 2,
};

/** Request services implemented by the in-process Server. */
enum class RequestService : std::uint16_t {
    /** Uploads activity-selection state and gets one runtime activity-session id back. */
    activityHostManager = 6,
    /** Carries one authenticated client-to-activity-host message. */
    activityMessage = 8,
    /** Carries one client-to-server Web Service request envelope. */
    webService = 10,
    /** Carries one Web Service request on the server-role channel, answered on 112. */
    webServiceServer = 110,
    /** Registers one family and SOID selector for later pushes. */
    subscribeFamily = 12,
    /** Releases one previously declared family root. */
    unsubscribeFamily = 14,
    /** Requests a relay endpoint for one activity-host id. */
    activityHost = 16,
    /** Requests the current client-configuration response. */
    clientConfig = 18,
    /** Requests the current purchased-offers result set. */
    purchasedOffers = 21,
    /** Turns one request id into the active account SOID. */
    accountTranslation = 23,
    /** Exchanges the SignOn token for secure-channel parameters. */
    serverHello = 25,
    /** Carries a one-way client notification with no response service. */
    notification29 = 29,
    /** Starts the plaintext channel with a fixed nonce echo. */
    start = 30,
    /** Requests the current server-to-client user-message response. */
    userMessage = 32,
    /** Requests the skill records the Client keeps in its skill manager. */
    skill = 34,
    /** Carries the unnamed request paired with response service 37. */
    request36 = 36,
    /** Carries the unnamed request paired with response service 39. */
    request38 = 38,
    /** Carries the unnamed request paired with response service 41. */
    request40 = 40,
    /** Carries one of the 8 matchmaking request variants. Body field two picks it. */
    matchmaking = 42,
    /** Carries the unnamed request paired with response service 49. */
    request48 = 48,
    /** Carries a clan protobuf request that the minimal liveness route leaves unparsed. */
    clan = 44,
    /** Registers the client as a notification subscriber. */
    registerSubscriber = 121,
    /** Carries a large one-way client notification with no response service. */
    notification171 = 171,
    /** Keeps an authenticated connection active. */
    echo = 250,
    /** Registers the client with the relay service. */
    registerRelayClient = 302,
    /** Wraps the client-owned Steam certificate for the response. */
    signSteamCertificate = 304,
    /** Carries the opaque request paired with response service 307. */
    accountFromMembership = 306,
};

/** Response services emitted by the in-process Server. */
enum class ResponseService : std::uint16_t {
    /** Returns one runtime activity-session id and the least activity data. */
    activityHostManager = 7,
    /** Returns one server-to-client Web Service response envelope. */
    webService = 11,
    /** Returns one Web Service response on the server-role channel for request 110. */
    webServiceServer = 112,
    /** Acknowledges a family subscription with an empty status-200 body. */
    subscribeFamily = 13,
    /** Acknowledges a family unsubscribe with an empty status-200 body. */
    unsubscribeFamily = 15,
    /** Returns a relay endpoint for one activity-host id. */
    activityHost = 17,
    /** Returns the current client-configuration fields. */
    clientConfig = 19,
    /** Returns the purchased-offers result set. */
    purchasedOffers = 22,
    /** Returns one request id paired with the active account SOID. */
    accountTranslation = 24,
    /** Returns secure-channel parameters after SignOn token validation. */
    serverHello = 26,
    /** Echoes the fixed channel-start nonce. */
    start = 31,
    /** Returns the current user-message fields. */
    userMessage = 33,
    /** Returns an empty skill record list, which is a count of zero. */
    skill = 35,
    /** Acknowledges service 36 with an empty status-200 body. */
    response37 = 37,
    /** Acknowledges service 38 with an empty status-200 body. */
    response39 = 39,
    /** Acknowledges service 40 with an empty status-200 body. */
    response41 = 41,
    /** Returns the request-kind-specific matchmaking result. */
    matchmaking = 43,
    /** Acknowledges service 48 with an empty status-200 body. */
    response49 = 49,
    /** Returns schema-valid empty clan data with status 200. */
    clan = 45,
    /** Acknowledges notification subscriber registration. */
    registerSubscriber = 122,
    /** Acknowledges an authenticated keepalive. */
    echo = 251,
    /** Acknowledges relay registration. */
    registerRelayClient = 303,
    /** Returns the client-owned Steam certificate wrapper. */
    signSteamCertificate = 305,
    /** Acknowledges service 306 with an empty status-200 body. */
    accountFromMembership = 307,
};

/** Server-initiated services emitted without a response status field. */
enum class NotificationService : std::uint16_t {
    /** Publishes one uncorrelated activity-host message. */
    activityMessage = 9,
    /** Publishes one or more queuez family updates. */
    queuezUpdate = 123,
};

/** Parsed BAP request header and borrowed body. */
struct RequestFrame {
    FrameType frameType{};
    std::uint16_t messageId{};
    std::uint32_t taskId{};
    std::span<const std::byte> body{};
};

/** Parsed outer BAP frame and borrowed payload. */
struct OuterFrame {
    FrameType frameType{};
    std::span<const std::byte> payload{};
};

/** Reads one BAP outer header and borrows the payload its length names. */
[[nodiscard]] bool parse_frame(std::span<const std::byte> input, OuterFrame& frame) noexcept;

/** Parses one decrypted or plaintext BAP request payload. */
[[nodiscard]] bool parse_request_payload(std::span<const std::byte> input,
                                         FrameType frameType,
                                         RequestFrame& request) noexcept;

/** Parses one complete plaintext BAP request frame. */
[[nodiscard]] bool parse_request(std::span<const std::byte> input, RequestFrame& request) noexcept;

/** Encodes one plaintext BAP response frame with status 200. */
[[nodiscard]] bool encode_response(ResponseService service,
                                   std::uint32_t taskId,
                                   FrameType frameType,
                                   std::span<const std::byte> body,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

/** Encodes a BAP response inner header and body. */
[[nodiscard]] bool encode_response_payload(ResponseService service,
                                           std::uint32_t taskId,
                                           std::span<const std::byte> body,
                                           std::span<std::byte> output,
                                           std::size_t& written) noexcept;

/**
 * Encodes one 6-byte notification header and body. There is no response status field.
 * @param sequence Server-chosen notification sequence.
 * @param output Caller-owned payload storage.
 * @param written Receives encoded payload bytes.
 * @return True when sizes fit the wire length and output storage.
 */
[[nodiscard]] bool encode_notification_payload(NotificationService service,
                                               std::uint32_t sequence,
                                               std::span<const std::byte> body,
                                               std::span<std::byte> output,
                                               std::size_t& written) noexcept;

/** Encodes one BAP outer frame around an existing payload. */
[[nodiscard]] bool encode_frame(FrameType frameType,
                                std::span<const std::byte> payload,
                                std::span<std::byte> output,
                                std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap
