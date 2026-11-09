#include "studio_command_link.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../../middleware/studio/studio_mapping.h"
#include "../../state/build_data/runtime/build_data_invalidation.h"
#include "../../state/mods/mods_runtime.h"
#include "../activity/mission/mission_script_runtime.h"
#include "../bap/runtime.h"
#include "studio_command_protocol.h"
#include "studio_tag_reload.h"

namespace sunrise::server::studio {
namespace {

namespace wire = command_wire;
namespace mission = activity::mission;

HANDLE g_mapping{};
wire::SharedCommand* g_view{};
SRWLOCK g_linkLock{SRWLOCK_INIT};
std::uint64_t g_nextConnect{};
/** The request id being served, and the reload ticket that will answer it. */
std::uint32_t g_servingRequest{};
std::uint64_t g_ticket{};
std::uint32_t g_editorPid{};
std::array<char, wire::kModIdBytes> g_requestedMod{};
/** True while the request being served waits for the tag reload worker. */
bool g_tagRequest{};

/** The bridge lock keeps a requester check from reading an unmapped view. */
std::uint64_t detach() noexcept {
    const auto ticket = g_ticket;
    middleware::studio::close_editor_view(g_mapping, g_view);
    g_servingRequest = 0;
    g_ticket = 0;
    g_editorPid = 0;
    g_requestedMod = {};
    g_tagRequest = false;
    return ticket;
}

/** Version 2 keeps the version 1 layout, so an editor of either version is served. */
[[nodiscard]] bool header_ok(const wire::SharedCommand& view) noexcept {
    return middleware::studio::editor_header_ok(
               view, wire::kMagic, wire::kVersion, wire::kMappingBytes)
           || middleware::studio::editor_header_ok(
               view, wire::kMagic, wire::kOldestVersion, wire::kMappingBytes);
}

/** Service and commit guards run on the same server thread. */
void disconnect() noexcept {
    static_cast<void>(mission::cancel_reload(detach()));
}

/** @return True only when an editor created a mapping of this exact protocol. */
[[nodiscard]] bool connect() noexcept {
    g_view =
        middleware::studio::open_editor_view<wire::SharedCommand>(wire::kMappingFormat, g_mapping);
    if (g_view == nullptr) {
        return false;
    }
    if (!header_ok(*g_view)) {
        disconnect();
        return false;
    }
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     "ev=studio stage=command result=connected");
    return true;
}

/** Writes the reply, then the response id, so Studio never reads half a reply. */
void answer(std::uint32_t request,
            wire::Status status,
            std::uint32_t reloaded,
            std::string_view message) noexcept {
    g_view->status = status;
    g_view->gamePid = GetCurrentProcessId();
    g_view->reloaded = reloaded;
    g_view->message = {};
    std::copy_n(message.data(),
                (std::min)(message.size(), wire::kMessageBytes - 1),
                g_view->message.data());
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_view->response),
                        static_cast<LONG>(request));
    g_servingRequest = 0;
    g_ticket = 0;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=studio stage=command result=answered request=%u "
                                      "status=%u reloaded=%u",
                                      request,
                                      static_cast<unsigned>(status),
                                      reloaded);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @return True when a mod with this id loaded this session. */
[[nodiscard]] bool mod_loaded(std::string_view id) noexcept {
    struct Lookup final {
        std::string_view id;
        bool found{};
    } lookup{id};
    static_cast<void>(state::mods::visit(
        [](void* context, const state::mods::View& view) noexcept {
            auto& search = *static_cast<Lookup*>(context);
            for (std::size_t index = 0; index < view.loadedCount; ++index) {
                search.found = search.found || view.records[index].manifest.id.view() == search.id;
            }
            return true;
        },
        &lookup));
    return lookup.found;
}

/** Only bridge state is read; the caller already holds the reload mailbox lock. */
bool requester_present(std::uint64_t ticket) noexcept {
    AcquireSRWLockShared(&g_linkLock);
    bool present = false;
    if (ticket == g_ticket && g_view != nullptr && g_servingRequest != 0
        && core::settings::get().studioAttach) {
        const auto request = static_cast<std::uint32_t>(
            InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_view->request), 0, 0));
        present = header_ok(*g_view) && g_view->editorPid == g_editorPid
                  && g_view->kind == wire::Kind::reloadMod && g_view->modId == g_requestedMod
                  && request == g_servingRequest
                  && middleware::studio::editor_alive(
                      g_view->heartbeat, GetTickCount64(), wire::kHeartbeatTimeoutMs)
                  && static_cast<std::uint32_t>(InterlockedCompareExchange(
                         reinterpret_cast<volatile LONG*>(&g_view->response), 0, 0))
                         != request
                  && static_cast<std::uint32_t>(InterlockedCompareExchange(
                         reinterpret_cast<volatile LONG*>(&g_view->request), 0, 0))
                         == request;
    }
    ReleaseSRWLockShared(&g_linkLock);
    return present;
}

/** Checks one new request and hands a valid reload to the mission runtime. */
void start(std::uint32_t request) noexcept {
    static_cast<void>(mission::cancel_reload(g_ticket));
    g_ticket = 0;
    g_servingRequest = request;
    std::array<char, wire::kModIdBytes> modId = g_view->modId;
    g_requestedMod = modId;
    g_editorPid = g_view->editorPid;
    modId.back() = '\0';
    const std::string_view id(modId.data());
    g_tagRequest = false;
    const wire::Kind kind = g_view->kind;
    if (id.empty()) {
        answer(request, wire::Status::invalid, 0, "empty mod id");
        return;
    }
    switch (kind) {
    case wire::Kind::reloadMod:
        break;
    case wire::Kind::reloadBubble: {
        std::array<char, wire::kMessageBytes> message = g_view->message;
        message.back() = '\0';
        const tag_reload::Reply reply = tag_reload::reload_bubble(std::string_view(message.data()));
        answer(request, reply.status, 0, reply.message.data());
        return;
    }
    case wire::Kind::reloadTags:
    case wire::Kind::patchTags:
    case wire::Kind::queryResident:
        if (kind != wire::Kind::queryResident && !mod_loaded(id)) {
            answer(request, wire::Status::notLoaded, 0, "the mod did not load this session");
            return;
        }
        // The worker answers on a later slice; the manifest stays mapped by Studio until then.
        if (!tag_reload::start(g_editorPid, request, kind)) {
            answer(request, wire::Status::failed, 0, "a tag reload is still running");
            return;
        }
        g_tagRequest = true;
        return;
    default:
        answer(request, wire::Status::invalid, 0, "unknown command");
        return;
    }
    if (!mod_loaded(id)) {
        answer(request, wire::Status::notLoaded, 0, "the mod did not load this session");
        return;
    }
    g_ticket = mission::request_reload(id, &requester_present);
}

/** @return The wire status for one answered reload. */
[[nodiscard]] wire::Status wire_status(mission::ReloadStatus status) noexcept {
    switch (status) {
    case mission::ReloadStatus::ok:
        return wire::Status::ok;
    case mission::ReloadStatus::disabled:
        return wire::Status::disabled;
    case mission::ReloadStatus::compileError:
        return wire::Status::compileError;
    case mission::ReloadStatus::pending:
    case mission::ReloadStatus::failed:
    case mission::ReloadStatus::replaced:
    case mission::ReloadStatus::cancelled:
        break;
    }
    return wire::Status::failed;
}

/** Each request id gets exactly one reply, written when the mission runtime has answered it. */
void service_locked(std::uint64_t now) noexcept {
    // Off by default: no mapping is ever opened.
    if (!core::settings::get().studioAttach) {
        if (g_view != nullptr) {
            disconnect();
        }
        return;
    }
    if (g_view == nullptr && now >= g_nextConnect) {
        g_nextConnect = now + wire::kConnectIntervalMs;
        static_cast<void>(connect());
    }
    if (g_view == nullptr) {
        return;
    }
    if (!middleware::studio::editor_alive(g_view->heartbeat, now, wire::kHeartbeatTimeoutMs)) {
        disconnect();
        return;
    }
    const auto request = static_cast<std::uint32_t>(
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_view->request), 0, 0));
    const auto response = static_cast<std::uint32_t>(
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_view->response), 0, 0));
    if (request == 0 || request == response) {
        return;
    }
    if (request != g_servingRequest) {
        start(request);
        if (g_ticket == 0 && !g_tagRequest) {
            return;
        }
    }
    if (g_tagRequest) {
        tag_reload::Reply reply{};
        if (!tag_reload::take(request, reply)) {
            return;
        }
        g_tagRequest = false;
        // The new tables are published; the dropped domains rebuild on the next investment slice.
        if (reply.domains != 0) {
            state::build_data::invalidation::invalidate(reply.domains);
            bap::request_investment_slice();
        }
        answer(request, reply.status, reply.applied, reply.message.data());
        return;
    }
    const mission::ReloadResult result = mission::reload_result(g_ticket);
    if (result.status == mission::ReloadStatus::pending) {
        return;
    }
    answer(request,
           wire_status(result.status),
           result.reopened,
           result.status == mission::ReloadStatus::replaced
               ? std::string_view("a newer reload request replaced this one")
               : std::string_view(result.message.data()));
}

} // namespace

/** Mapping ownership stays fixed for the entire command service slice. */
void service(std::uint64_t now) noexcept {
    AcquireSRWLockExclusive(&g_linkLock);
    service_locked(now);
    ReleaseSRWLockExclusive(&g_linkLock);
}

/** Release the bridge lock before waiting for an in-progress mailbox commit. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_linkLock);
    const auto ticket = detach();
    g_nextConnect = 0;
    ReleaseSRWLockExclusive(&g_linkLock);
    static_cast<void>(mission::cancel_reload(ticket));
    tag_reload::stop();
}

} // namespace sunrise::server::studio
