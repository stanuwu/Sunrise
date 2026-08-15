#include <Windows.h>

#include <array>
#include <atomic>

#include "../../core/logging/log.h"
#include "../../state/matchmaking/matchmaking_state.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::server::bap {
namespace {

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Session, kSessionCount> g_sessions{};
Scratch g_scratch{};

/** @param id Nonzero connection id. @return Matching open session, or null. */
[[nodiscard]] Session* session_for(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return nullptr;
    }
    auto& session = g_sessions[id - 1];
    return session.id == id ? &session : nullptr;
}

/** @param session Its secrets and identity are wiped. */
void clear_session(Session& session) noexcept {
    SecureZeroMemory(&session, sizeof session);
}

/**
 * Releases an authenticated session's optional matchmaking context.
 * @param session Open session that may not have finished server hello.
 * @return True when there was no context, or the active generation was released.
 */
[[nodiscard]] bool release_matchmaking_context(Session& session) noexcept {
    if (session.matchmakingContext.generation == state::matchmaking::kInvalidGeneration) {
        session.matchmakingContext = {};
        return true;
    }
    if (!state::matchmaking::release_context(session.matchmakingContext)) {
        return false;
    }
    session.matchmakingContext = {};
    return true;
}

/** @param id Session-slot id. @return True when the slot is opened. */
[[nodiscard]] bool open_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    clear_session(session);
    session.id = id;
    return true;
}

/** @param id Session-slot id. @return True when the slot is cleared. */
[[nodiscard]] bool close_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    clear_session(session);
    return true;
}

/**
 * Routes one validated frame through its connection-owned session.
 * @param request Frame event and caller-owned buffers.
 * @param response Receives encoded response size.
 * @return True when the frame is valid and its service is handled.
 */
[[nodiscard]] bool consume_frame(const client::network::BapRequest& request,
                                 client::network::BapResponse& response) noexcept {
    middleware::bap::OuterFrame frame;
    if (!middleware::bap::parse_frame(request.frame, frame)) {
        return false;
    }
    auto* session = session_for(request.connectionId);
    if (session == nullptr) {
        return false;
    }
    bool handled = false;
    if (frame.frameType == middleware::bap::FrameType::encrypted) {
        handled = encrypted::consume(*session, g_scratch, frame, request.response, response.size);
    } else {
        handled = plaintext::consume(*session, g_scratch, frame, request.response, response.size);
    }
    if (!handled) {
        return false;
    }
    // A frame response can carry one already-due push in the same bounded socket write.
    bool touchesScratch = true;
    std::size_t deferred = 0;
    if (response.size < request.response.size()
        && encrypted::consume_deferred(*session,
                                       g_scratch,
                                       request.response.subspan(response.size),
                                       deferred,
                                       touchesScratch)) {
        response.size += deferred;
    }
    return true;
}

/**
 * Services one timed poll for a session that may owe a deferred push.
 * @param request Poll event and caller-owned output buffer.
 * @param response Receives encoded notification size.
 * @param touchesScratch Set when the attempt reaches a scratch buffer.
 * @return True when a notification is published.
 */
[[nodiscard]] bool consume_poll(const client::network::BapRequest& request,
                                client::network::BapResponse& response,
                                bool& touchesScratch) noexcept {
    static std::atomic_bool reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::info, "ev=queuez stage=poll result=ok");
    }
    auto* session = session_for(request.connectionId);
    return session != nullptr
           && encrypted::consume_deferred(
               *session, g_scratch, request.response, response.size, touchesScratch);
}

} // namespace

/** Applies one serialized BAP connection lifecycle event. */
bool consume(const client::network::BapRequest& request,
             client::network::BapResponse& response) noexcept {
    response = {};
    AcquireSRWLockExclusive(&g_lock);
    bool success = false;
    // Polls report whether they reached scratch.
    bool touchesScratch = request.event != client::network::BapEvent::poll;
    // Hold the session lock across cryptographic counter reads and updates.
    switch (request.event) {
    case client::network::BapEvent::open:
        success = open_session(request.connectionId);
        break;
    case client::network::BapEvent::frame:
        success = consume_frame(request, response);
        break;
    case client::network::BapEvent::poll:
        success = consume_poll(request, response, touchesScratch);
        break;
    case client::network::BapEvent::close:
        success = close_session(request.connectionId);
        break;
    }
    // Decrypted frames can contain runtime-only keys or tokens, so scratch never outlives the call.
    if (touchesScratch) {
        SecureZeroMemory(&g_scratch, sizeof g_scratch);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return success;
}

/** Securely erases every connection-owned nonce and transform buffer. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (auto& session : g_sessions) {
        if (session.id != 0
            && session.matchmakingContext.generation != state::matchmaking::kInvalidGeneration) {
            // State erases runtime descriptors before the opaque association is cleared.
            (void)state::matchmaking::release_context(session.matchmakingContext);
        }
    }
    SecureZeroMemory(g_sessions.data(), sizeof g_sessions);
    SecureZeroMemory(&g_scratch, sizeof g_scratch);
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::bap
