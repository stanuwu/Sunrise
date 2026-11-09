#include "studio_link.h"

#include <Windows.h>

#include "../../../core/settings/settings.h"
#include "../../../middleware/studio/studio_mapping.h"
#include "../../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../hooks/teleport/runtime.h"
#include "../../player/player_position.h"
#include "studio_cui_link.h"
#include "studio_object_link.h"
#include "studio_pose_link.h"
#include "studio_protocol.h"
#include "studio_world_link.h"

namespace sunrise::client::diagnostics::studio {
namespace {

/** Native handles use all bits set for none. */
constexpr std::uint32_t kNoHandle = 0xFFFFFFFFU;

HANDLE g_mapping{};
SharedSnapshot* g_view{};
std::uint64_t g_nextConnect{};
SRWLOCK g_lock{SRWLOCK_INIT};
bool g_accepting{};

/** Closes both mapping resources while the caller owns g_lock. */
void disconnect() noexcept {
    middleware::studio::close_editor_view(g_mapping, g_view);
}

/** @return True when an editor has created a compatible mapping for this process. */
[[nodiscard]] bool connect() noexcept {
    g_view = middleware::studio::open_editor_view<SharedSnapshot>(kMappingFormat, g_mapping);
    if (g_view == nullptr) {
        return false;
    }
    if (!middleware::studio::editor_header_ok(*g_view, kMagic, kVersion, sizeof(SharedSnapshot))) {
        disconnect();
        return false;
    }
    return true;
}

/** Publishes one frame without retaining game pointers or accepting game mutations. */
void publish(std::uint64_t now) noexcept {
    const bool world = hooks::bootflow::in_world();
    const auto slice = hooks::bootflow::current_slice_set();
    hooks::teleport::Vector player{};
    void* const component = client::player::position::component();
    const bool playerValid = world && component != nullptr
                             && hooks::teleport::owns_local_player(component)
                             && hooks::teleport::read_position(component, player);
    hooks::teleport::CameraPose camera{};
    const bool cameraValid = world && hooks::teleport::camera_pose(camera);
    std::uint32_t playerHandle = 0;
    const bool handleValid = world && hooks::teleport::controlled_handle(playerHandle);
    std::uint32_t flags = world ? inWorld : 0;
    if (playerValid) {
        flags |= playerPresent;
    }
    if (handleValid) {
        flags |= playerHandlePresent;
    }
    if (cameraValid) {
        flags |= cameraPresent;
    }
    if (slice.available) {
        flags |= sliceAvailable;
    }
    if (slice.available && slice.present) {
        flags |= slicePresent;
    }
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_view->sequence));
    g_view->frameTime = now;
    g_view->gamePid = GetCurrentProcessId();
    g_view->flags = flags;
    g_view->currentSlice = slice.available && slice.present ? slice.index : -1;
    g_view->player = player;
    g_view->cameraPosition = camera.position;
    g_view->cameraForward = camera.forward;
    g_view->cameraUp = camera.up;
    g_view->horizontalFov = camera.horizontalFov;
    g_view->aspect = camera.aspect;
    g_view->playerHandle = handleValid ? playerHandle : 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_view->sequence));
    set_world_player(handleValid ? playerHandle : kNoHandle);
}

} // namespace

/** Setup is serialized with camera callbacks that may already be installed. */
void initialize() noexcept {
    // Off by default: no observer is installed and no mapping is ever opened.
    if (!core::settings::get().studioAttach) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    (void)initialize_world_link();
    (void)initialize_pose_link();
    (void)initialize_cui_link();
    g_accepting = true;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Skips concurrent camera calls so the shared sequence always has one writer. */
void poll() noexcept {
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        return;
    }
    if (!g_accepting) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (g_view == nullptr && now >= g_nextConnect) {
        g_nextConnect = now + kConnectInterval;
        (void)connect();
    }
    if (g_view != nullptr) {
        if (!middleware::studio::editor_alive(g_view->heartbeat, now, kHeartbeatTimeout)) {
            // The sub-links are polled only from here, so they close with the core view.
            disconnect();
            drop_world_link();
            drop_pose_link();
            drop_object_link();
        } else {
            publish(now);
            poll_world_link();
            poll_pose_link();
            poll_object_link();
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Stops frame publication before detaching observers or clearing their reader targets. */
bool close() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_accepting = false;
    disconnect();
    close_object_link();
    if (!close_pose_link() || !close_cui_link()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    close_world_link();
    g_nextConnect = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

} // namespace sunrise::client::diagnostics::studio
