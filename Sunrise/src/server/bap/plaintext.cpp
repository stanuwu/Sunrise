#include <Windows.h>

#include <array>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../../middleware/secure_channel/runtime.h"
#include "../../state/account/account_token.h"
#include "../../state/coop/channel_key.h"
#include "../../state/coop/compatibility.h"
#include "../../state/matchmaking/matchmaking_state.h"
#include "../../state/runtime/runtime.h"
#include "../../state/social/steam_roster.h"
#include "account_enrollment.h"
#include "coop_admission.h"
#include "encrypted/internal.h"
#include "internal.h"
#include "proxy/proxy_runtime.h"

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
    session.sessionKey = bap.sessionKey;
    if (!session.authenticated && core::settings::hosts_session()) {
        state::social::session_directory().opened(session.accountHandle);
    }
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
    (void)encrypted::routing::resolve(request.serviceId, route);
    if (proxy::classify(request.serviceId, request.body) == proxy::Plane::upstream) {
        return route.responseMode == encrypted::ResponseMode::reply
                   ? proxy::forward_plaintext_request(session.id,
                                                      request.serviceId,
                                                      static_cast<std::uint16_t>(route.response),
                                                      request.taskId,
                                                      request.body)
                   : proxy::forward_uncorrelated(
                         session.id, request.serviceId, request.taskId, request.body);
    }
    if (route.responseMode != encrypted::ResponseMode::reply) {
        return true;
    }
    encrypted::ServiceOutcome outcome{};
    std::size_t bodySize = 0;
    // A codec that refuses answers with an empty body. The Client matches only the head of its
    // pending ring, so one unanswered request jams that ring for the rest of the run.
    if (!encrypted::body::process(route,
                                  session.queuez,
                                  session.activity,
                                  session.activityRosterDecode,
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
    outcome = {};
    if (!encoded) {
        report_refusal(session, request.serviceId, "encode");
        return false;
    }
    return true;
}

/** What a guest's hello is, once checked against this host. */
enum class GuestKind : std::uint8_t {
    /** The guest's own Sunrise link; its keys come from the handshake. */
    sunrise,
    /** A native link of a guest whose Sunrise link reported its keys from the same address. */
    native,
    /** The link stays unauthenticated and closes. */
    refused,
    /** The reply already names the failed check; the link stays unauthenticated. */
    refusedWithReply,
};

/** One checked guest hello. */
struct GuestHello final {
    GuestKind kind{GuestKind::refused};
    state::coop::channel_key::PublicKey guestPublic{};
    /** The guest's own sign-on keys, for a native link. */
    state::SignOnState wrap{};
};

/**
 * Checks one guest hello. A Sunrise link carries a summary and a handshake key and is compared; a
 * native link is accepted only from the address the guest's Sunrise link reported keys from.
 * @param session Connection the hello arrived on.
 * @param scratch Lock-owned staging for the refusal body.
 * @param frame The hello.
 * @param token The hello's sign-on token, already bounds-checked.
 * @param response Whole-frame storage for a refusal reply.
 * @param written Receives the refusal reply's size.
 * @return The verdict, with the guest's key or sign-on keys.
 */
[[nodiscard]] GuestHello check_guest(const Session& session,
                                     Scratch& scratch,
                                     const middleware::bap::RequestFrame& frame,
                                     std::span<const std::byte> token,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept {
    namespace compatibility = state::coop::compatibility;
    GuestHello output{};
    const std::uint64_t primarySoid = state::account::soid_from_signon_token(token);
    const compatibility::Record* host = compatibility::local();
    if (primarySoid == 0 || host == nullptr) {
        return output;
    }
    std::span<const std::byte> summary{}, publicKey{};
    const bool sunriseLink = frame.body.size() > kServerHelloRequestSize
                             && frame.body[kProtocolTagOffset] == kProtocolVersionTag
                             && frame.body[kProtocolVersionOffset] == kProtocolVersion
                             && compatibility::find_hello_field(
                                 frame.body.subspan(kServerHelloRequestSize), summary, publicKey);
    if (!sunriseLink) {
        if (coop_admission::keys(primarySoid, session.remoteAddress, output.wrap)) {
            output.kind = GuestKind::native;
        }
        return output;
    }
    compatibility::Record guest{};
    if (!compatibility::decode_summary(summary, guest)) {
        return output;
    }
    const compatibility::Mismatch mismatch = compatibility::compare(guest, *host);
    core::log::writef(core::log::Channel::server,
                      mismatch == compatibility::Mismatch::none ? core::log::Level::info
                                                                : core::log::Level::warn,
                      "ev=coop stage=hello result=%s conn=%u from=0x%08X mismatch=%s",
                      mismatch == compatibility::Mismatch::none ? "ok" : "refused",
                      session.id,
                      session.remoteAddress,
                      compatibility::mismatch_name(mismatch));
    if (mismatch == compatibility::Mismatch::none) {
        std::copy(publicKey.begin(), publicKey.end(), output.guestPublic.begin());
        output.kind = GuestKind::sunrise;
        return output;
    }
    std::size_t bodySize = 0;
    if (compatibility::encode_refusal(*host, mismatch, scratch.responseBody, bodySize)
        && middleware::bap::encode_response(middleware::bap::ResponseService::serverHello,
                                            frame.taskId,
                                            frame.frameType,
                                            std::span(scratch.responseBody).first(bodySize),
                                            response,
                                            written)) {
        output.kind = GuestKind::refusedWithReply;
    } else {
        written = 0;
    }
    return output;
}

/**
 * Answers a guest's Sunrise link: a fresh host key, the keys both ends derive from the exchange,
 * and the tag that proves this host derived them.
 * @return False when the exchange or the reply failed.
 */
[[nodiscard]] bool accept_sunrise_link(const middleware::bap::RequestFrame& frame,
                                       const state::coop::channel_key::PublicKey& guestPublic,
                                       state::BapState& bapState,
                                       std::span<std::byte> response,
                                       std::size_t& written) noexcept {
    namespace channel_key = state::coop::channel_key;
    const auto* local = state::coop::compatibility::local();
    channel_key::ecdh::KeyPair hostKey;
    channel_key::PublicKey hostPublic{};
    channel_key::Confirm confirm{};
    std::array<std::byte, channel_key::kAcceptSize> accept{};
    const bool derived =
        local != nullptr && hostKey.generate() && hostKey.public_key(hostPublic)
        && channel_key::derive(
            hostKey, guestPublic, local->joinSecret, guestPublic, hostPublic, bapState, confirm);
    if (derived) {
        channel_key::encode_accept(hostPublic, confirm, accept);
    }
    return derived
           && middleware::bap::encode_response(middleware::bap::ResponseService::serverHello,
                                               frame.taskId,
                                               frame.frameType,
                                               accept,
                                               response,
                                               written);
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
    if (frame.serviceId == static_cast<std::uint16_t>(middleware::bap::RequestService::start)) {
        const bool encoded =
            middleware::bap::encode_response(middleware::bap::ResponseService::start,
                                             frame.taskId,
                                             frame.frameType,
                                             frame.body,
                                             response,
                                             written);
        return encoded;
    }
    if (frame.serviceId
        != static_cast<std::uint16_t>(middleware::bap::RequestService::serverHello)) {
        if (core::settings::hosts_session() && !session.authenticated) {
            return false;
        }
        return consume_service(session, scratch, frame, response, written);
    }

    const auto& signOnState = state::sign_on();
    state::AccountHandle accountHandle = state::kLocalAccount;
    AccountEnrollment enrollment;
    GuestHello guest{};
    guest.kind = GuestKind::native;
    guest.wrap = signOnState;
    if (core::settings::hosts_session()) {
        // Activity-channel hellos may carry more fields after the same bounded token prefix.
        if (frame.body.size() < kTokenOffset + state::kSessionTokenSize
            || frame.body[0] != kSessionTokenTag || frame.body[1] != kSessionTokenSize) {
            report_refusal(session, frame.serviceId, "account_token");
            return false;
        }
        const auto token = frame.body.subspan(kTokenOffset, state::kSessionTokenSize);
        if (!state::is_local_account_token(token)) {
            guest = check_guest(session, scratch, frame, token, response, written);
            if (guest.kind == GuestKind::refusedWithReply) {
                return true;
            }
            if (guest.kind == GuestKind::refused) {
                report_refusal(session, frame.serviceId, "coop_unverified");
                return false;
            }
        }
        if (!enroll_account(token, accountHandle, enrollment.attachedNow)) {
            report_refusal(session, frame.serviceId, "account_token");
            return false;
        }
        enrollment.handle = accountHandle;
        enrollment.primary = state::account_primary_soid(accountHandle);
        if ((session.authenticated && accountHandle != session.accountHandle)
            // 127.0.0.1 in host order: the local account may only be claimed from this machine.
            || (accountHandle == state::kLocalAccount && session.remoteAddress != 0x7F000001U)) {
            report_refusal(session, frame.serviceId, "account_scope");
            return false;
        }
    }
    const state::ScopedAccount accountScope(accountHandle);
    // This connection's own key, nonce and envelope IV. The envelope hands them to the client, so
    // the material it carries and the material this link then seals with are the same object.
    state::BapState bapState{};
    if (!state::new_bap_session(bapState)) {
        report_refusal(session, frame.serviceId, "session_material");
        return false;
    }
    // Local channels retain the upstream framing exception: the activity host's hello uses
    // other framing, and refusing it strands that link in `_authenticating`. Shared-service
    // channels must first resolve the bounded account token above.
    if (session.authenticated) {
        report_refusal(session, frame.serviceId, "reauthenticated");
    }
    if (!core::settings::hosts_session() && !valid_hello_shape(frame.body)) {
        report_hello_shape(session, frame.body);
    } else if (!core::settings::hosts_session() && !hello_token_matches(frame.body, signOnState)) {
        report_refusal(session, frame.serviceId, "token_mismatch_accepted");
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
        report_refusal(session, frame.serviceId, "context_unavailable");
    }
    std::array<std::byte, middleware::secure_channel::kServerHelloEnvelopeSize> envelope{};
    std::size_t envelopeSize = 0;
    // A native link gets its session key wrapped with its own install's sign-on keys.
    const bool encoded =
        guest.kind == GuestKind::sunrise
            ? accept_sunrise_link(frame, guest.guestPublic, bapState, response, written)
            : middleware::secure_channel::encode_server_hello(
                  guest.wrap, bapState, envelope, envelopeSize)
                  && middleware::bap::encode_response(middleware::bap::ResponseService::serverHello,
                                                      frame.taskId,
                                                      frame.frameType,
                                                      std::span(envelope).first(envelopeSize),
                                                      response,
                                                      written);
    SecureZeroMemory(envelope.data(), envelope.size());
    SecureZeroMemory(&guest.wrap, sizeof guest.wrap);
    if (!encoded) {
        (void)state::matchmaking::release_context(matchmakingContext);
        report_refusal(session, frame.serviceId, "encode");
        return false;
    }
    // Publish the context and counters only after the whole svc-26 response exists. A context that
    // was not free publishes as invalid instead of costing the reply.
    session.matchmakingContext = matchmakingContext;
    session.accountHandle = accountHandle;
    session.sunriseLink = guest.kind == GuestKind::sunrise;
    if (session.authenticated && core::settings::get().server.upstream.enabled) {
        proxy::close_link(session.id);
        proxy::open_link(session.id);
    }
    arm_encryption(session, bapState);
    enrollment.attachedNow = false;
    return true;
}

} // namespace sunrise::server::bap::plaintext
