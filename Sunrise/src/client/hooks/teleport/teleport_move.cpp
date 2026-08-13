/**
 * Teleport and noclip movement. The camera hook publishes a forward vector and polls controls once
 * a frame. The physics hook applies movement before the sync it runs ahead of. Physics owns the
 * position, so writing the object placement would move the camera alone.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../teleport/teleport_settings_store.h"
#include "../polled_input/runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::hooks::teleport {
namespace {

using Action = state::account::settings::bindings::Action;
using Clock = std::chrono::steady_clock;

/** Frames a request or delta survives without a player physics consumer. */
constexpr std::uint32_t kRequestLifetimeFrames = 3;
/** Frames an ordinary physics tick gets to collect movement before the forced path takes it. */
constexpr std::uint32_t kForceAfterFrames = 1;
/** Frames the injected press is held so it survives one scan and one integration step. */
constexpr std::uint32_t kPressFrames = 2;
/** A delayed frame cannot turn one held movement key into a large jump. */
constexpr float kMaximumFrameSeconds = 0.05F;
/** Squared-vector floor used before normalization. */
constexpr float kVectorEpsilon = 0.000001F;

/** Authored action driven to wake the body when no native movement key is already effective. */
constexpr std::uint16_t kForwardAction = static_cast<std::uint16_t>(Action::moveForward);

enum class NoclipControl : std::size_t {
    forward,
    backward,
    left,
    right,
    rise,
    descend,
    boost,
    count,
};

constexpr std::size_t kNoclipControlCount = static_cast<std::size_t>(NoclipControl::count);

std::atomic_bool g_requested{false};
std::atomic_bool g_forwardValid{false};
std::atomic_bool g_keyDown{false};
std::atomic_uint32_t g_requestAge{0};
/** Set while one-shot teleport is usable, keeping its idle hook path inexpensive. */
std::atomic_bool g_active{false};

std::atomic_bool g_noclipActive{false};
std::atomic_bool g_noclipToggleDown{false};
std::atomic_bool g_noclipMovePending{false};
std::atomic_uint32_t g_noclipMoveAge{0};

/**
 * The player's physics component, kept from the last tick that carried it. At rest the sync stops
 * being called for the player, so the pointer is also the forced path back to them.
 */
std::atomic<std::byte*> g_playerComponent{nullptr};
/** Frames left before the injected press is released. */
std::atomic_uint32_t g_pressFrames{0};

ControlledHandle g_controlledHandle{};
CameraSingleton g_cameraSingleton{};

/** Camera and physics hooks execute on the same game thread, so these vectors need only gates. */
std::array<float, kVectorLanes> g_forward{};
std::array<float, kVectorLanes> g_pendingNoclipDelta{};
std::array<float, kVectorLanes> g_noclipPosition{};
std::array<float, kVectorLanes> g_previousValidRight{0.0F, 1.0F, 0.0F};
std::array<std::uint32_t, kNoclipControlCount> g_noclipKeys{};

std::byte* g_noclipBody{};
bool g_noclipPositionValid{};
Clock::time_point g_lastNoclipPoll{};
bool g_noclipClockValid{};

/** Reads one value out of game memory without faulting on a torn pointer. */
template <typename T> [[nodiscard]] bool read_at(const std::byte* address, T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

/** Writes one three-lane vector into game memory. */
[[nodiscard]] bool write_vector(std::byte* address,
                                const std::array<float, kVectorLanes>& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T written = 0;
    const SIZE_T size = sizeof(float) * kVectorLanes;
    return WriteProcessMemory(GetCurrentProcess(), address, value.data(), size, &written) != FALSE
           && written == size;
}

/** @return The rigid body driven by a physics component, or null when the chain breaks. */
[[nodiscard]] std::byte* body_of(std::byte* component) noexcept {
    std::byte* array = nullptr;
    std::int32_t index = 0;
    if (!read_at(component + kPhysicsComponentBodyArray, array)
        || !read_at(component + kPhysicsComponentBodyIndex, index) || array == nullptr
        || index < 0) {
        return nullptr;
    }
    std::byte* body = nullptr;
    const std::size_t offset = kBodyEntryStride * static_cast<std::size_t>(index) + kBodyPointer;
    return read_at(array + offset, body) ? body : nullptr;
}

/** @param reason Key naming the step that stopped movement. */
void report_skip(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=teleport stage=move result=skip reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the gates the original sync tests before publishing a transform. */
void report_gates(const std::byte* component, const std::byte* body) noexcept {
    std::uint8_t suppressed = 0;
    std::int32_t bodyIndex = 0;
    std::uint32_t bodyFlags = 0;
    std::uint8_t motionType = 0;
    (void)read_at(component + kPhysicsComponentSuppress, suppressed);
    (void)read_at(component + kPhysicsComponentBodyIndex, bodyIndex);
    (void)read_at(body + kBodyFlags, bodyFlags);
    (void)read_at(body + kBodyMotionType, motionType);
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=teleport stage=gates suppress=%u index=%d "
                                      "flags=0x%08X active=%u motion=%u",
                                      static_cast<unsigned>(suppressed),
                                      static_cast<int>(bodyIndex),
                                      static_cast<unsigned>(bodyFlags),
                                      (bodyFlags & kBodyActiveBit) != 0 ? 1U : 0U,
                                      static_cast<unsigned>(motionType));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @return True when a component drives the object the local player controls. */
[[nodiscard]] bool owns_player(std::byte* component) noexcept {
    if (component == nullptr || g_controlledHandle == nullptr) {
        return false;
    }
    std::uint32_t controlled = kInvalidHandle;
    g_controlledHandle(&controlled);
    if (controlled == kInvalidHandle) {
        return false;
    }
    std::uint16_t owner = 0;
    return read_at(component + kPhysicsComponentObjectHandle, owner)
           && (controlled & kHandleIndexMask)
                  == (static_cast<std::uint32_t>(owner) & kHandleIndexMask);
}

/** @return True while a controlled player object exists. */
[[nodiscard]] bool controlled_player_available() noexcept {
    if (g_controlledHandle == nullptr) {
        return false;
    }
    std::uint32_t controlled = kInvalidHandle;
    g_controlledHandle(&controlled);
    return controlled != kInvalidHandle;
}

void clear_noclip_delta() noexcept {
    g_pendingNoclipDelta = {};
    g_noclipMoveAge.store(0, std::memory_order_relaxed);
    g_noclipMovePending.store(false, std::memory_order_release);
}

void clear_noclip_target() noexcept {
    g_noclipPosition = {};
    g_noclipBody = nullptr;
    g_noclipPositionValid = false;
}

/** Invalidates every pointer and displacement that must not cross a destination transition. */
void clear_player_state() noexcept {
    g_playerComponent.store(nullptr, std::memory_order_relaxed);
    clear_noclip_delta();
    clear_noclip_target();
    g_noclipClockValid = false;
}

void set_noclip_active(bool active) noexcept {
    const bool changed = g_noclipActive.exchange(active, std::memory_order_acq_rel) != active;
    if (!active) {
        clear_noclip_delta();
        clear_noclip_target();
        g_noclipKeys = {};
        g_noclipClockValid = false;
    }
    if (changed) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         active ? "ev=noclip stage=toggle active=1"
                                : "ev=noclip stage=toggle active=0");
    }
}

/** Ages and drops a one-shot teleport that no player tick collected. */
void expire_request() noexcept {
    if (!g_requested.load(std::memory_order_acquire)) {
        return;
    }
    if (g_requestAge.fetch_add(1, std::memory_order_relaxed) + 1 >= kRequestLifetimeFrames) {
        g_requested.store(false, std::memory_order_release);
    }
}

/** Ages and drops a noclip delta that no player tick collected. */
void expire_noclip_delta() noexcept {
    if (!g_noclipMovePending.load(std::memory_order_acquire)) {
        return;
    }
    if (g_noclipMoveAge.fetch_add(1, std::memory_order_relaxed) + 1 >= kRequestLifetimeFrames) {
        clear_noclip_delta();
    }
}

/** Stops the synthetic wake press once it has survived the required game scans. */
void end_press() noexcept {
    if (g_pressFrames.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (g_pressFrames.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
        hooks::polled_input::release_key();
    }
}

/** Resolves one action's primary key, falling back to its secondary binding. */
[[nodiscard]] std::uint32_t bound_key(const state::AccountState& account, Action action) noexcept {
    const auto& binding = account.settings.keyBindings.values[static_cast<std::size_t>(action)];
    if (binding.primary.has_value()) {
        const std::uint32_t key = action_key(*binding.primary);
        if (key != 0) {
            return key;
        }
    }
    return binding.secondary.has_value() ? action_key(*binding.secondary) : 0;
}

/** Resolves movement bindings once per noclip activation. */
void resolve_noclip_controls() noexcept {
    const state::AccountState account = state::account_snapshot();
    g_noclipKeys[static_cast<std::size_t>(NoclipControl::forward)] =
        bound_key(account, Action::moveForward);
    g_noclipKeys[static_cast<std::size_t>(NoclipControl::backward)] =
        bound_key(account, Action::moveBackward);
    g_noclipKeys[static_cast<std::size_t>(NoclipControl::left)] =
        bound_key(account, Action::moveLeft);
    g_noclipKeys[static_cast<std::size_t>(NoclipControl::right)] =
        bound_key(account, Action::moveRight);
    g_noclipKeys[static_cast<std::size_t>(NoclipControl::rise)] = bound_key(account, Action::jump);

    std::uint32_t descend = bound_key(account, Action::holdCrouch);
    if (descend == 0) {
        descend = bound_key(account, Action::toggleCrouch);
    }
    g_noclipKeys[static_cast<std::size_t>(NoclipControl::descend)] = descend;

    std::uint32_t boost = bound_key(account, Action::holdSprint);
    if (boost == 0) {
        boost = bound_key(account, Action::toggleSprint);
    }
    g_noclipKeys[static_cast<std::size_t>(NoclipControl::boost)] = boost;
}

/** Starts or refreshes the synthetic forward press that wakes an otherwise sleeping body. */
void begin_press() noexcept {
    std::uint32_t virtualKey = 0;
    if (g_noclipActive.load(std::memory_order_relaxed)) {
        virtualKey = g_noclipKeys[static_cast<std::size_t>(NoclipControl::forward)];
    } else {
        const state::AccountState account = state::account_snapshot();
        const auto& binding = account.settings.keyBindings.values[kForwardAction];
        if (binding.primary.has_value()) {
            virtualKey = action_key(*binding.primary);
        }
    }
    if (virtualKey == 0) {
        report_skip("no_key");
        return;
    }
    hooks::polled_input::hold_key(virtualKey);
    g_pressFrames.store(kPressFrames, std::memory_order_release);
}

/** Writes one vertical velocity, leaving ordinary run momentum on the other two lanes. */
void set_vertical_velocity(std::byte* body, float value) noexcept {
    std::array<float, kVectorLanes> velocity{};
    if (!read_at(body + kBodyVelocityX, velocity)) {
        return;
    }
    velocity[kVerticalLane] = value;
    (void)write_vector(body + kBodyVelocityX, velocity);
}

/** Adds one world delta to a stored vector. */
[[nodiscard]] bool offset_vector(std::byte* address,
                                 const std::array<float, kVectorLanes>& delta,
                                 std::array<float, kVectorLanes>& before,
                                 std::array<float, kVectorLanes>& after) noexcept {
    if (!read_at(address, before)) {
        return false;
    }
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        after[lane] = before[lane] + delta[lane];
    }
    return write_vector(address, after);
}

/** Performs the original one-shot move outside noclip. */
[[nodiscard]] bool move_body(std::byte* body, float distance) noexcept {
    std::array<float, kVectorLanes> delta{};
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        delta[lane] = g_forward[lane] * distance;
    }
    std::array<float, kVectorLanes> position{};
    std::array<float, kVectorLanes> moved{};
    if (!offset_vector(body + kBodyPositionX, delta, position, moved)) {
        report_skip("body");
        return false;
    }
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=teleport stage=move result=ok dist=%.1f "
                                      "from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f",
                                      static_cast<double>(distance),
                                      static_cast<double>(position[0]),
                                      static_cast<double>(position[1]),
                                      static_cast<double>(position[2]),
                                      static_cast<double>(moved[0]),
                                      static_cast<double>(moved[1]),
                                      static_cast<double>(moved[2]));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/** Runs the complete original one-shot move for a proven player component. */
[[nodiscard]] bool perform_teleport(std::byte* component) noexcept {
    std::byte* const body = body_of(component);
    if (body == nullptr) {
        report_skip("no_body");
        return false;
    }
    report_gates(component, body);
    set_vertical_velocity(body, 0.0F);
    if (!move_body(body, client::teleport::get().distance)) {
        return false;
    }
    begin_press();
    return true;
}

struct MoveResult {
    bool written{};
    bool moved{};
};

/**
 * Replaces the body position with the stored noclip target. Physics output is never used as the
 * next-frame origin after target initialization.
 */
[[nodiscard]] MoveResult perform_noclip(std::byte* component, bool teleportRequested) noexcept {
    std::byte* const body = body_of(component);
    if (body == nullptr) {
        clear_noclip_target();
        report_skip("no_body");
        return {};
    }
    if (g_noclipPositionValid && body != g_noclipBody) {
        // A body replacement is also a lifecycle boundary. A delta authored for the old body must
        // never be replayed against the replacement.
        clear_noclip_delta();
        clear_noclip_target();
    }
    if (!g_noclipPositionValid || body != g_noclipBody) {
        std::array<float, kVectorLanes> position{};
        if (!read_at(body + kBodyPositionX, position)) {
            clear_noclip_target();
            report_skip("body");
            return {};
        }
        g_noclipPosition = position;
        g_noclipBody = body;
        g_noclipPositionValid = true;
        report_gates(component, body);
    }

    const bool deltaPending = g_noclipMovePending.load(std::memory_order_acquire);
    std::array<float, kVectorLanes> candidate = g_noclipPosition;
    if (deltaPending) {
        for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
            candidate[lane] += g_pendingNoclipDelta[lane];
        }
    }
    if (teleportRequested) {
        const float distance = client::teleport::get().distance;
        for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
            candidate[lane] += g_forward[lane] * distance;
        }
    }

    if (!write_vector(body + kBodyPositionX, candidate)) {
        report_skip("body");
        return {};
    }
    g_noclipPosition = candidate;
    if (deltaPending) {
        clear_noclip_delta();
    }

    constexpr std::array<float, kVectorLanes> stopped{};
    if (!write_vector(body + kBodyVelocityX, stopped)) {
        report_skip("velocity");
    }

    const bool moved = deltaPending || teleportRequested;
    if (moved) {
        begin_press();
    }
    return MoveResult{true, moved};
}

/** Applies all ready movement to a component already proven to own the player. */
[[nodiscard]] MoveResult apply_player_movement(std::byte* component) noexcept {
    const bool requested =
        g_active.load(std::memory_order_relaxed) && g_requested.load(std::memory_order_acquire);
    const bool teleportReady = requested && g_forwardValid.load(std::memory_order_acquire);
    if (g_noclipActive.load(std::memory_order_acquire)) {
        const MoveResult result = perform_noclip(component, teleportReady);
        if (teleportReady && result.written) {
            g_requested.store(false, std::memory_order_release);
        }
        return result;
    }
    if (!teleportReady) {
        return {};
    }
    g_requested.store(false, std::memory_order_release);
    const bool moved = perform_teleport(component);
    return MoveResult{moved, moved};
}

/** @return The key state Sunrise sees; polled-input suppression only changes game callers. */
[[nodiscard]] bool key_held(NoclipControl control) noexcept {
    const std::uint32_t key = g_noclipKeys[static_cast<std::size_t>(control)];
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
}

[[nodiscard]] float length_squared(const std::array<float, kVectorLanes>& value) noexcept {
    float length = 0.0F;
    for (float lane : value) {
        length += lane * lane;
    }
    return length;
}

[[nodiscard]] std::array<float, kVectorLanes>
normalized(const std::array<float, kVectorLanes>& value) noexcept {
    const float squared = length_squared(value);
    if (squared <= kVectorEpsilon) {
        return {};
    }
    const float inverse = 1.0F / std::sqrt(squared);
    std::array<float, kVectorLanes> result{};
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        result[lane] = value[lane] * inverse;
    }
    return result;
}

/** Adds this frame's camera-relative movement to the pending world delta. */
void poll_noclip_movement(const client::teleport::Settings& settings) noexcept {
    const Clock::time_point now = Clock::now();
    if (!g_noclipClockValid) {
        g_lastNoclipPoll = now;
        g_noclipClockValid = true;
        return;
    }
    const float frameSeconds = std::clamp(
        std::chrono::duration<float>(now - g_lastNoclipPoll).count(), 0.0F, kMaximumFrameSeconds);
    g_lastNoclipPoll = now;
    if (frameSeconds <= 0.0F || !g_forwardValid.load(std::memory_order_acquire)) {
        return;
    }

    std::array<float, kVectorLanes> forward = normalized(g_forward);
    if (length_squared(forward) <= kVectorEpsilon) {
        return;
    }
    std::array<float, kVectorLanes> planarForward{forward[0], forward[1], 0.0F};
    if (length_squared(planarForward) > kVectorEpsilon) {
        planarForward = normalized(planarForward);
        g_previousValidRight = {planarForward[1], -planarForward[0], 0.0F};
    }
    const std::array<float, kVectorLanes>& right = g_previousValidRight;

    const float forwardInput = (key_held(NoclipControl::forward) ? 1.0F : 0.0F)
                               - (key_held(NoclipControl::backward) ? 1.0F : 0.0F);
    const float rightInput = (key_held(NoclipControl::right) ? 1.0F : 0.0F)
                             - (key_held(NoclipControl::left) ? 1.0F : 0.0F);
    const float verticalInput = (key_held(NoclipControl::rise) ? 1.0F : 0.0F)
                                - (key_held(NoclipControl::descend) ? 1.0F : 0.0F);

    std::array<float, kVectorLanes> direction{};
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        direction[lane] = forward[lane] * forwardInput + right[lane] * rightInput;
    }
    direction[kVerticalLane] += verticalInput;
    if (length_squared(direction) > 1.0F) {
        direction = normalized(direction);
    }
    if (length_squared(direction) <= kVectorEpsilon) {
        return;
    }

    float speed = settings.noclipSpeed;
    if (key_held(NoclipControl::boost)) {
        speed *= settings.noclipBoostMultiplier;
    }
    const bool alreadyPending = g_noclipMovePending.load(std::memory_order_acquire);
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        g_pendingNoclipDelta[lane] += direction[lane] * speed * frameSeconds;
    }
    if (!alreadyPending) {
        g_noclipMoveAge.store(0, std::memory_order_relaxed);
        g_noclipMovePending.store(true, std::memory_order_release);
    }
}

} // namespace

/** Publishes the two functions the hooks call. */
void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept {
    g_controlledHandle = controlled;
    g_cameraSingleton = singleton;
}

/** Drops target functions and every latched request, pointer, and noclip target. */
void clear_targets() noexcept {
    g_controlledHandle = nullptr;
    g_cameraSingleton = nullptr;
    g_requested.store(false, std::memory_order_release);
    g_forwardValid.store(false, std::memory_order_release);
    g_keyDown.store(false, std::memory_order_relaxed);
    g_requestAge.store(0, std::memory_order_relaxed);
    g_active.store(false, std::memory_order_relaxed);
    g_noclipToggleDown.store(false, std::memory_order_relaxed);
    set_noclip_active(false);
    clear_player_state();
}

/** Publishes the camera forward vector for the physics tick that follows. */
void capture_forward(std::uint32_t playerIndex) noexcept {
    if (playerIndex == kInvalidHandle || g_cameraSingleton == nullptr) {
        g_forwardValid.store(false, std::memory_order_release);
        clear_player_state();
        return;
    }
    std::byte* const camera = g_cameraSingleton();
    if (camera == nullptr) {
        g_forwardValid.store(false, std::memory_order_release);
        clear_player_state();
        return;
    }
    std::array<float, kVectorLanes> forward{};
    if (!read_at(camera + kCameraBlockStride * playerIndex + kCameraForwardX, forward)) {
        g_forwardValid.store(false, std::memory_order_release);
        return;
    }
    g_forward = forward;
    g_forwardValid.store(true, std::memory_order_release);
}

/** Polls the one-shot key, noclip toggle, and active camera-relative movement. */
void poll_controls() noexcept {
    end_press();
    expire_request();
    expire_noclip_delta();

    const client::teleport::Settings settings = client::teleport::get();
    const bool teleportUsable = settings.enabled && settings.virtualKey != client::teleport::kNoKey;
    g_active.store(teleportUsable, std::memory_order_relaxed);
    if (!teleportUsable) {
        g_requested.store(false, std::memory_order_release);
        g_requestAge.store(0, std::memory_order_relaxed);
    }
    const bool interfaceVisible = core::ui::runtime::snapshot().visible;

    if (!controlled_player_available()) {
        clear_player_state();
    } else {
        std::byte* const cached = g_playerComponent.load(std::memory_order_relaxed);
        if (cached != nullptr && !owns_player(cached)) {
            clear_player_state();
        }
    }

    if (!teleportUsable || interfaceVisible) {
        g_keyDown.store(false, std::memory_order_relaxed);
    } else {
        const bool down = (GetAsyncKeyState(static_cast<int>(settings.virtualKey)) & 0x8000) != 0;
        if (down && !g_keyDown.exchange(down, std::memory_order_relaxed)) {
            g_requestAge.store(0, std::memory_order_relaxed);
            g_requested.store(true, std::memory_order_release);
        } else {
            g_keyDown.store(down, std::memory_order_relaxed);
        }
    }

    const bool noclipUsable =
        settings.noclipEnabled && settings.noclipToggleKey != client::teleport::kNoKey;
    if (!noclipUsable) {
        g_noclipToggleDown.store(false, std::memory_order_relaxed);
        set_noclip_active(false);
        return;
    }
    if (interfaceVisible) {
        g_noclipToggleDown.store(false, std::memory_order_relaxed);
        g_noclipClockValid = false;
        return;
    }

    const bool toggleDown =
        (GetAsyncKeyState(static_cast<int>(settings.noclipToggleKey)) & 0x8000) != 0;
    if (toggleDown && !g_noclipToggleDown.exchange(toggleDown, std::memory_order_relaxed)) {
        const bool activate = !g_noclipActive.load(std::memory_order_acquire);
        if (activate) {
            resolve_noclip_controls();
            clear_noclip_delta();
            clear_noclip_target();
            g_noclipClockValid = false;
        }
        set_noclip_active(activate);
    } else {
        g_noclipToggleDown.store(toggleDown, std::memory_order_relaxed);
    }

    if (!g_noclipActive.load(std::memory_order_acquire)) {
        return;
    }
    // No component means orbit/loading or the first pre-sync frame. Do not queue movement that a
    // later spawn could consume; the active physics hook will discover and cache the next body.
    if (g_playerComponent.load(std::memory_order_relaxed) == nullptr) {
        g_noclipClockValid = false;
        return;
    }
    poll_noclip_movement(settings);
}

/** @return True while noclip is toggled on. */
bool noclip_active() noexcept {
    return g_noclipActive.load(std::memory_order_acquire);
}

/** Applies pending movement before the original sync publishes the player's transform. */
void apply_pending(void* component) noexcept {
    if (component == nullptr || g_controlledHandle == nullptr) {
        return;
    }
    const bool requested =
        g_active.load(std::memory_order_relaxed) && g_requested.load(std::memory_order_acquire);
    const bool noclip = g_noclipActive.load(std::memory_order_acquire);
    if (!requested && !noclip) {
        return;
    }

    std::byte* const physics = static_cast<std::byte*>(component);
    std::byte* const cached = g_playerComponent.load(std::memory_order_relaxed);
    if (cached != nullptr && physics != cached) {
        return;
    }
    if (!owns_player(physics)) {
        if (physics == cached) {
            clear_player_state();
        }
        return;
    }
    g_playerComponent.store(physics, std::memory_order_relaxed);
    (void)apply_player_movement(physics);
}

/** Runs movement through the cached component when no ordinary physics tick collected it. */
void force_pending() noexcept {
    const bool teleportReady = g_active.load(std::memory_order_relaxed)
                               && g_requested.load(std::memory_order_acquire)
                               && g_forwardValid.load(std::memory_order_acquire)
                               && g_requestAge.load(std::memory_order_relaxed) >= kForceAfterFrames;
    const bool noclipReady =
        g_noclipMovePending.load(std::memory_order_acquire)
        && g_noclipMoveAge.load(std::memory_order_relaxed) >= kForceAfterFrames;
    if (!teleportReady && !noclipReady) {
        return;
    }

    std::byte* const physics = g_playerComponent.load(std::memory_order_relaxed);
    if (physics == nullptr || g_controlledHandle == nullptr || !owns_player(physics)) {
        clear_player_state();
        return;
    }
    const MoveResult result = apply_player_movement(physics);
    if (!result.written || !result.moved) {
        return;
    }
    invoke_sync(physics);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=teleport stage=force result=ok");
}

} // namespace sunrise::client::hooks::teleport
