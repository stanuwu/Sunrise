/**
 * Selectable fly. Velocity mode drives the body's velocity through the simulation. Coordinate mode
 * moves its position after the simulation, bypassing collision without requiring noclip.
 */

#include "fly.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../noclip/runtime.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::fly {
namespace {

namespace bindings = state::account::settings::bindings;

/** The high bit of a polled key state marks it held. */
constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
/** Below this squared length a direction counts as none. */
constexpr float kMinimumLengthSquared = 0.000001F;
/** A resumed or stalled frame must not turn into one large coordinate jump. */
constexpr float kMaximumStepSeconds = 0.05F;

/** The horizontal lanes. The basis is X forward, Z up. */
constexpr std::size_t kLaneX = 0;
constexpr std::size_t kLaneY = 1;

/** One movement direction in the player's own frame. */
enum class Direction : std::size_t {
    forward,
    backward,
    left,
    right,
    up,
    down,
    count,
};

/** Sizes the per-direction flag array. */
constexpr std::size_t kDirectionCount = static_cast<std::size_t>(Direction::count);

/** One polled action and the direction it moves. */
struct ActionDirection {
    bindings::Action action;
    Direction direction;
};

/** Crouch has two actions and either one descends. Both are read as a hold, to pair with jump. */
constexpr std::array<ActionDirection, 7> kActions{{
    {bindings::Action::moveForward, Direction::forward},
    {bindings::Action::moveBackward, Direction::backward},
    {bindings::Action::moveLeft, Direction::left},
    {bindings::Action::moveRight, Direction::right},
    {bindings::Action::jump, Direction::up},
    {bindings::Action::toggleCrouch, Direction::down},
    {bindings::Action::holdCrouch, Direction::down},
}};

/** Both binding halves of each polled action, in the order of the table above. */
std::array<bindings::Binding, kActions.size()> g_bindings{};
/** Set once the bindings are read, so the costly account snapshot runs once. */
bool g_bindingsRead{false};
/** The toggle key on the previous frame, so the switch only flips on the press. */
std::atomic_bool g_toggleDown{false};
/** The height the body had before the step, which is the height to hold. */
float g_heightBeforeStep{0.0F};
bool g_heightValid{false};
/** Set while a press owns the vertical lane. The hold stands aside. */
bool g_steered{false};
/** Coordinate target authored before the step and written after collision resolution. */
noclip::Vector g_coordinateTarget{};
bool g_coordinateTargetValid{false};

/**
 * Takes the account's movement bindings once they are loaded.
 * @return True when they have been read.
 */
[[nodiscard]] bool read_bindings() noexcept {
    if (g_bindingsRead) {
        return true;
    }
    // The snapshot copies the whole account, so it stops once the bindings arrive.
    const state::AccountState account = state::account_snapshot();
    if (!account.settings.keyBindings.configured) {
        return false;
    }
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const std::size_t action = static_cast<std::size_t>(kActions[index].action);
        g_bindings[index] = account.settings.keyBindings.values[action];
    }
    g_bindingsRead = true;
    return true;
}

/**
 * A half is an input code, not a virtual key. A half bound to a mouse button gives no key.
 * @param half One binding half, empty when unbound.
 * @return True while its key is down.
 */
[[nodiscard]] bool half_down(const std::optional<std::uint16_t>& half) noexcept {
    if (!half.has_value()) {
        return false;
    }
    const std::uint32_t key = teleport::action_key(*half);
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kKeyHeldBit) != 0;
}

/** @return One flag per direction, true while any key bound to it is down. */
[[nodiscard]] std::array<bool, kDirectionCount> pressed_directions() noexcept {
    std::array<bool, kDirectionCount> pressed{};
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const bindings::Binding& binding = g_bindings[index];
        if (half_down(binding.primary) || half_down(binding.secondary)) {
            pressed[static_cast<std::size_t>(kActions[index].direction)] = true;
        }
    }
    return pressed;
}

/**
 * Forward turned about the up axis. This turn is the player's right, confirmed in flight.
 * @return The strafe axis, or zeroes when the camera looks straight up or down.
 */
[[nodiscard]] teleport::Vector right_of(const teleport::Vector& forward) noexcept {
    teleport::Vector right{forward[kLaneY], -forward[kLaneX], 0.0F};
    const float lengthSquared = right[kLaneX] * right[kLaneX] + right[kLaneY] * right[kLaneY];
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    const float length = std::sqrt(lengthSquared);
    right[kLaneX] /= length;
    right[kLaneY] /= length;
    return right;
}

/**
 * Composes the pressed directions into one unit vector.
 * @param pressed One flag per direction.
 * @param forward Camera forward vector.
 * @return The direction to fly, or all zeroes when nothing is pressed.
 */
[[nodiscard]] teleport::Vector travel(const std::array<bool, kDirectionCount>& pressed,
                                      const teleport::Vector& forward) noexcept {
    const teleport::Vector right = right_of(forward);
    teleport::Vector move{};
    const auto add = [&move](const teleport::Vector& axis, float scale) noexcept {
        for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
            move[lane] += axis[lane] * scale;
        }
    };
    if (pressed[static_cast<std::size_t>(Direction::forward)]) {
        add(forward, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::backward)]) {
        add(forward, -1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::right)]) {
        add(right, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::left)]) {
        add(right, -1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::up)]) {
        move[teleport::kVerticalLane] += 1.0F;
    }
    if (pressed[static_cast<std::size_t>(Direction::down)]) {
        move[teleport::kVerticalLane] -= 1.0F;
    }
    float lengthSquared = 0.0F;
    for (const float lane : move) {
        lengthSquared += lane * lane;
    }
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    // Normalised, so a diagonal is not faster than a straight line.
    const float length = std::sqrt(lengthSquared);
    for (float& lane : move) {
        lane /= length;
    }
    return move;
}

/**
 * Shortens a velocity to a speed limit, keeping its direction.
 * @param velocity Velocity to limit in place.
 * @param limit Highest speed to leave.
 */
void cap_speed(teleport::Vector& velocity, float limit) noexcept {
    float speedSquared = 0.0F;
    for (const float lane : velocity) {
        speedSquared += lane * lane;
    }
    if (speedSquared <= limit * limit) {
        return;
    }
    const float scale = limit / std::sqrt(speedSquared);
    for (float& lane : velocity) {
        lane *= scale;
    }
}

/**
 * Works out the velocity the keys ask for. Also records whether a press owns the vertical lane.
 * @param speed Configured fly speed.
 * @return Velocity in world units per second.
 */
[[nodiscard]] teleport::Vector desired_velocity(float speed) noexcept {
    // The interface or another application owns the keyboard. Their presses must not steer.
    std::array<bool, kDirectionCount> pressed{};
    if (!core::ui::runtime::snapshot().visible && input::game_focused()) {
        pressed = pressed_directions();
    }
    teleport::Vector forward{};
    const teleport::Vector move =
        teleport::camera_forward(forward) ? travel(pressed, forward) : teleport::Vector{};
    g_steered = move[teleport::kVerticalLane] != 0.0F;
    teleport::Vector velocity{};
    for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
        velocity[lane] = move[lane] * speed;
    }
    return velocity;
}

} // namespace

/** Reads the toggle key once a frame and flips the switch on the press. */
void poll_toggle() noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (settings.flyToggleKey == client::movement::kNoKey) {
        g_toggleDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down =
        input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.flyToggleKey)) & kKeyHeldBit) != 0;
    // The interface owns the keyboard, so the key tracks the press but never flips the switch.
    if (core::ui::runtime::snapshot().visible) {
        g_toggleDown.store(down, std::memory_order_relaxed);
        return;
    }
    if (down && !g_toggleDown.exchange(true, std::memory_order_acq_rel)) {
        client::movement::Settings updated = settings;
        updated.flyEnabled = !settings.flyEnabled;
        if (!client::movement::publish(updated)) {
            return;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         updated.flyEnabled ? "ev=fly stage=toggle enabled=1"
                                            : "ev=fly stage=toggle enabled=0");
        return;
    }
    if (!down) {
        g_toggleDown.store(false, std::memory_order_release);
    }
}

/** Writes the player's velocity on the physics sync, which publishes it. */
void apply(void* component) noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (!settings.flyEnabled) {
        return;
    }
    if (component == nullptr || !teleport::owns_local_player(component)) {
        return;
    }
    if (!read_bindings()) {
        return;
    }
    teleport::Vector velocity{};
    if (settings.flyMode == client::movement::FlyMode::velocity) {
        // Capped, because this is the field the game reads to decide the player hit something too
        // hard. The step has the real speed; this is only what the sync publishes.
        velocity = desired_velocity(settings.flySpeed);
        cap_speed(velocity, kPublishedSpeedCap);
    }
    (void)teleport::write_velocity(component, velocity);
}

/** Reports whether fly is on. */
bool enabled() noexcept {
    return client::movement::get().flyEnabled;
}

/** Prepares the selected fly mode for the coming simulation step. */
void before_step(void* body, float deltaTime) noexcept {
    g_heightValid = false;
    g_coordinateTargetValid = false;
    if (body == nullptr || !read_bindings()) {
        return;
    }
    const client::movement::Settings settings = client::movement::get();
    const teleport::Vector velocity = desired_velocity(settings.flySpeed);
    if (settings.flyMode == client::movement::FlyMode::coordinate) {
        noclip::read_body_position(body, g_coordinateTarget);
        const float step = std::clamp(deltaTime, 0.0F, kMaximumStepSeconds);
        for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
            g_coordinateTarget[lane] += velocity[lane] * step;
        }
        noclip::write_body_velocity(body, noclip::Vector{});
        g_coordinateTargetValid = true;
        return;
    }
    noclip::write_body_velocity(body, velocity);
    noclip::Vector position{};
    noclip::read_body_position(body, position);
    g_heightBeforeStep = position[teleport::kVerticalLane];
    g_heightValid = true;
}

/** Completes coordinate movement or puts back the height gravity took in velocity mode. */
void after_step(void* body, bool heldElsewhere) noexcept {
    if (body != nullptr && g_coordinateTargetValid) {
        noclip::write_body_position(body, g_coordinateTarget);
        noclip::write_body_velocity(body, noclip::Vector{});
        g_coordinateTargetValid = false;
        return;
    }
    if (body == nullptr || heldElsewhere || g_steered || !g_heightValid) {
        return;
    }
    // The height from before this step, so a respawn or any other placement is kept.
    noclip::Vector position{};
    noclip::read_body_position(body, position);
    position[teleport::kVerticalLane] = g_heightBeforeStep;
    noclip::write_body_position(body, position);
    // The step also left its gravity in the velocity, where it would build up.
    noclip::Vector velocity{};
    noclip::read_body_velocity(body, velocity);
    velocity[teleport::kVerticalLane] = 0.0F;
    noclip::write_body_velocity(body, velocity);
}

/** Clears the key state and the held height. The switch is a stored setting and survives. */
void reset() noexcept {
    g_toggleDown.store(false, std::memory_order_release);
    g_heightValid = false;
    g_coordinateTargetValid = false;
}

} // namespace sunrise::client::hooks::fly
