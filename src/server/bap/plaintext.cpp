#include <Windows.h>

#include <array>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/secure_channel/runtime.h"
#include "../../state/matchmaking/matchmaking_state.h"
#include "../../state/runtime/runtime.h"
#include "encrypted/internal.h"
#include "internal.h"

namespace sunrise::server::bap::plaintext {
namespace {

/** Protobuf field tag for the length-delimited session token. */
constexpr std::byte kSessionTokenTag{0x0A};
/** Protobuf length byte for the 32-byte session token. */
constexpr std::byte kSessionTokenSize{0x20};
/** Protobuf field tag for the varint protocol version. */
constexpr std::byte kProtocolVersionTag{0x10};
/** Supported server-hello protocol version. */
constexpr std::byte kProtocolVersion{0x01};
/** Fixed protobuf offsets derived from the two one-byte field headers. */
constexpr std::size_t kTokenOffset = 2;
constexpr std::size_t kProtocolTagOffset = kTokenOffset + state::kSessionTokenSize;
constexpr std::size_t kProtocolVersionOffset = kProtocolTagOffset + 1;
constexpr std::size_t kServerHelloRequestSize = kProtocolVersionOffset + 1;
/** Protocol direction bit applied to the final receive-nonce byte. */
constexpr std::byte kReceiveDirectionMask{0x01};

/**
 * Logs one refused plaintext request and names the check that refused it.
 * A refused server hello leaves its link stuck in `_authenticating` until the activity setup
 * times out. That timeout names no service, so this line is the only record of it.
 * @param session Session the request arrived on, owned by the connection.
 * @param service Numeric request service.
 * @param reason Short name of the check that refused.
 */
void report_refusal(const Session& session, std::uint16_t service, const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bap svc=%u stage=plaintext result=fail conn=%u "
                                      "reason=%s",
                                      static_cast<unsigned>(service),
                                      session.id,
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Logs a server-hello body that does not match the primary link's documented framing.
 * Only the framing bytes are named. The token they wrap is a secret and never reaches the log.
 * @param session Session the hello arrived on, owned by the connection.
 * @param body Plaintext service body.
 */
void report_hello_shape(const Session& session, std::span<const std::byte> body) noexcept {
    const auto at = [body](std::size_t index) noexcept {
        return index < body.size() ? std::to_integer<unsigned>(body[index]) : 0U;
    };
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bap svc=25 stage=plaintext result=shape_accepted "
                                      "conn=%u len=%zu b0=%02X b1=%02X b34=%02X b35=%02X",
                                      session.id,
                                      body.size(),
                                      at(0),
                                      at(1),
                                      at(kProtocolTagOffset),
                                      at(kProtocolVersionOffset));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Checks the server-hello framing, which is fixed and carries no secret.
 * @param body Plaintext service body.
 * @return True when the length, both field tags and the protocol version all match.
 */
[[nodiscard]] bool valid_hello_shape(std::span<const std::byte> body) noexcept {
    return body.size() == kServerHelloRequestSize && body[0] == kSessionTokenTag
           && body[1] == kSessionTokenSize && body[kProtocolTagOffset] == kProtocolVersionTag
           && body[kProtocolVersionOffset] == kProtocolVersion;
}

/**
 * Compares the echoed session token against the one SignOn issued.
 * @param body Plaintext service body whose framing is already checked.
 * @param signOn Active SignOn token state.
 * @return True when every token byte matches.
 */
[[nodiscard]] bool hello_token_matches(std::span<const std::byte> body,
                                       const state::SignOnState& signOn) noexcept {
    for (std::size_t index = 0; index < signOn.sessionToken.size(); ++index) {
        if (body[kTokenOffset + index] != signOn.sessionToken[index]) {
            return false;
        }
    }
    return true;
}

/**
 * Copies process BAP keys into one authenticated connection session.
 * @param session Crypto state owned by the connection.
 * @param bap Process BAP key and nonce material.
 */
void arm_encryption(Session& session, const state::BapState& bap) noexcept {
    session.sendNonce = bap.nonce;
    session.receiveNonce = bap.nonce;
    session.receiveNonce.back() ^= kReceiveDirectionMask;
    session.authenticated = true;
}

/**
 * Answers one plaintext request through the service table the encrypted path uses.
 * The route decides the reply just as it does for a decrypted frame, and one owing no response
 * writes nothing. Body side effects are not staged. The transports carrying them are encrypted.
 * @param session Queuez, activity and matchmaking bindings owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param request Parsed plaintext request header and borrowed body.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when the route owes nothing, or its whole response is encoded.
 */
[[nodiscard]] bool consume_service(Session& session,
                                   Scratch& scratch,
                                   const middleware::bap::RequestFrame& request,
                                   std::span<std::byte> response,
                                   std::size_t& written) noexcept {
    written = 0;
    encrypted::ServiceRoute route;
    (void)encrypted::routing::resolve(request.messageId, route);
    if (route.responseMode != encrypted::ResponseMode::reply) {
        core::log::write(core::log::Channel::server, core::log::Level::info, route.successEvent);
        return true;
    }
    encrypted::ServiceOutcome outcome{};
    std::size_t bodySize = 0;
    // A codec that refuses answers with an empty body. The Client matches only the head of its
    // pending ring, so one unanswered request jams that ring for the rest of the run.
    if (!encrypted::body::process(route,
                                  session.queuez,
                                  session.activitySessionId,
                                  session.matchmakingContext,
                                  request.body,
                                  scratch.responseBody,
                                  bodySize,
                                  outcome)) {
        bodySize = 0;
    }
    const bool encoded =
        middleware::bap::encode_response(route.response,
                                         request.taskId,
                                         request.frameType,
                                         std::span(scratch.responseBody).first(bodySize),
                                         response,
                                         written);
    SecureZeroMemory(scratch.responseBody.data(), bodySize);
    SecureZeroMemory(&outcome, sizeof outcome);
    if (!encoded) {
        report_refusal(session, request.messageId, "encode");
        return false;
    }
    core::log::write(core::log::Channel::server, core::log::Level::info, route.successEvent);
    return true;
}

} // namespace

/**
 * Handles plaintext channel start and server hello, and routes every other plaintext service.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when the service owes no reply, or its response is encoded.
 */
bool consume(Session& session,
             Scratch& scratch,
             const middleware::bap::OuterFrame& outer,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    written = 0;
    middleware::bap::RequestFrame frame;
    if (!middleware::bap::parse_request_payload(outer.payload, outer.frameType, frame)) {
        report_refusal(session, 0, "parse");
        return false;
    }
    // The channel-start body is a nonce the Client checks only the length of, so echoing whatever
    // arrived is always the correct reply.
    if (frame.messageId == static_cast<std::uint16_t>(middleware::bap::RequestService::start)) {
        const bool encoded =
            middleware::bap::encode_response(middleware::bap::ResponseService::start,
                                             frame.taskId,
                                             frame.frameType,
                                             frame.body,
                                             response,
                                             written);
        if (encoded) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             "ev=bap svc=30 rsp=31 result=ok");
        }
        return encoded;
    }
    if (frame.messageId
        != static_cast<std::uint16_t>(middleware::bap::RequestService::serverHello)) {
        return consume_service(session, scratch, frame, response, written);
    }

    const auto& signOnState = state::sign_on();
    const auto& bapState = state::bap();
    // Every service 25 is answered without reading the body, and that reaches the Tower on both
    // links. The reply is built from State alone, so the body is logged and never refused. The
    // activity host's hello uses other framing, and refusing it stranded that link in
    // `_authenticating` with no service named anywhere in the log.
    if (session.authenticated) {
        report_refusal(session, frame.messageId, "reauthenticated");
    }
    if (!valid_hello_shape(frame.body)) {
        report_hello_shape(session, frame.body);
    } else if (!hello_token_matches(frame.body, signOnState)) {
        report_refusal(session, frame.messageId, "token_mismatch_accepted");
    }
    // A repeat hello replaces its context, so the old one goes back before the new one is taken.
    // Holding both at once drains the small pool on the second hello of a session.
    if (session.matchmakingContext.generation != state::matchmaking::kInvalidGeneration) {
        (void)state::matchmaking::release_context(session.matchmakingContext);
        session.matchmakingContext = {};
    }
    state::matchmaking::ContextHandle matchmakingContext{};
    if (!state::matchmaking::acquire_context(matchmakingContext)) {
        // The svc-26 envelope is built from State alone. Refusing over a context the reply never
        // reads strands the link in its authenticating step with no service named anywhere.
        report_refusal(session, frame.messageId, "context_unavailable");
    }
    std::array<std::byte, middleware::secure_channel::kServerHelloEnvelopeSize> envelope{};
    std::size_t envelopeSize = 0;
    const bool encoded =
        middleware::secure_channel::encode_server_hello(
            signOnState, bapState, envelope, envelopeSize)
        && middleware::bap::encode_response(middleware::bap::ResponseService::serverHello,
                                            frame.taskId,
                                            frame.frameType,
                                            std::span(envelope).first(envelopeSize),
                                            response,
                                            written);
    SecureZeroMemory(envelope.data(), envelope.size());
    if (!encoded) {
        (void)state::matchmaking::release_context(matchmakingContext);
        report_refusal(session, frame.messageId, "encode");
        return false;
    }
    // Publish the context and counters only after the whole svc-26 response exists. A context that
    // was not free publishes as invalid instead of costing the reply.
    session.matchmakingContext = matchmakingContext;
    arm_encryption(session, bapState);
    core::log::write(
        core::log::Channel::server, core::log::Level::info, "ev=bap svc=25 rsp=26 result=ok");
    return true;
}

} // namespace sunrise::server::bap::plaintext
