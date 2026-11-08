/**
 * Noclip at the Havok simulation boundary. The hook reads the body's position and velocity before
 * simulation, lets Havok run, then writes the position on from where the body stood.
 * Collision resolution is discarded for the lanes it carries. The game keeps the position, so a
 * respawn or a teleport needs nothing reset here.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../hooking/detour.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_position.h"
#include "../fly/fly.h"
#include "../teleport/runtime.h"
#include "runtime.h"

namespace sunrise::client::hooks::noclip {
namespace {

/** hkpSimulation::stepDeltaTime, which encloses Havok integration and collision resolution. */
constexpr std::string_view kHavokStepText =
    "40 53 48 83 EC 20 83 79 38 01 48 8B D9 77 ? 48 8B 01 FF 50 20 F7 43 38 FD FF FF FF "
    "75 ? 48 8B 03 48 8B CB FF 50 28";
/** Masked form of the step text. This is the form the image scan takes. */
constexpr auto kHavokStep =
    patterns::signature<patterns::signature_length(kHavokStepText)>(kHavokStepText);

/** hkpSimulation::m_world. */
constexpr std::size_t kSimulationWorld = 0x18;
/** hkpWorld simulation-island arrays. */
constexpr std::array<std::size_t, 2> kWorldIslandArrays{0x40, 0x50};
/** hkpSimulationIsland::m_entities. */
constexpr std::size_t kIslandEntities = 0x60;
/** World position and linear velocity in the embedded motion. */
constexpr std::size_t kBodyPosition = 0x1C0;
constexpr std::size_t kBodyVelocity = 0x230;

/** X and Y are Destiny's horizontal world-space lanes, and Z the vertical one. */
constexpr std::size_t kHorizontalX = 0;
constexpr std::size_t kHorizontalY = 1;
constexpr std::size_t kVertical = 2;
constexpr std::size_t kVectorLanes = 4;

/**
 * Havok stores ownership flags in the upper two capacity bits. Masking them leaves the allocation
 * capacity used to validate an hkArray before walking it.
 */
constexpr std::uint32_t kArrayCapacityMask = 0x3FFFFFFF;
/** Defensive limits for game-owned island and entity arrays. */
constexpr std::int32_t kMaximumIslandCount = 4096;
constexpr std::int32_t kMaximumEntityCount = 65536;
/** Maximum accepted physics step; a resumed or stalled frame must not produce a large jump. */
constexpr float kMaximumStepSeconds = 0.05F;
/** Native velocity below this magnitude is treated as stationary. */
constexpr float kMinimumVelocitySquared = 0.000001F;

using HavokStep = std::int32_t(__fastcall*)(std::byte*, float);

struct HavokArray {
    std::byte** entries{};
    std::int32_t size{};
    std::uint32_t capacityAndFlags{};
};

/** hkArray packs its pointer, size and capacity into 16 bytes with no tail padding. */
constexpr std::size_t kHavokArrayBytes = 16;
static_assert(sizeof(HavokArray) == kHavokArrayBytes);

std::atomic_bool g_installed{false};
std::atomic_bool g_toggleDown{false};
hooking::detour::Handle g_stepHandle{};

/** Wall time between two fly measurement lines. */
constexpr ULONGLONG kMeasureWindowMs = 1000;

/** Fly movement summed over one window. Written by the step hook only. */
struct FlyMeasure {
    ULONGLONG windowStart{};
    std::uint32_t steps{};
    std::uint32_t found{};
    float simulated{};
    float asked{};
    float moved{};
    float leftOver{};
    std::int32_t islandMax{};
    bool noclip{};
    /** Body position before the window's last step. */
    std::array<float, kVectorLanes> position{};
};
FlyMeasure g_measure{};
/** Entity count of the island that held the player on the last lookup. */
std::int32_t g_playerIslandSize{};

/** Views one field while its owning Havok object is live inside the simulation hook. */
template <typename T> [[nodiscard]] T& field(std::byte* object, std::size_t offset) noexcept {
    return *reinterpret_cast<T*>(object + offset);
}

/** Copies the lanes the shorter vector carries, and leaves any beyond it alone. */
template <typename Source, typename Destination>
void copy_lanes(const Source& source, Destination& destination) noexcept {
    // The shorter vector's lane count, so a 3-lane caller never touches the stored fourth.
    constexpr std::size_t lanes =
        (std::min)(std::tuple_size_v<Source>, std::tuple_size_v<Destination>);
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        destination[lane] = source[lane];
    }
}

/**
 * Shortens a velocity to a speed limit, keeping its direction.
 * @param velocity Velocity to limit.
 * @param limit Highest speed to return.
 */
[[nodiscard]] std::array<float, kVectorLanes>
capped_speed(const std::array<float, kVectorLanes>& velocity, float limit) noexcept {
    const float speedSquared = velocity[kHorizontalX] * velocity[kHorizontalX]
                               + velocity[kHorizontalY] * velocity[kHorizontalY]
                               + velocity[kVertical] * velocity[kVertical];
    if (speedSquared <= limit * limit) {
        return velocity;
    }
    std::array<float, kVectorLanes> capped = velocity;
    const float scale = limit / std::sqrt(speedSquared);
    capped[kHorizontalX] *= scale;
    capped[kHorizontalY] *= scale;
    capped[kVertical] *= scale;
    return capped;
}

/** @return True when the array header is internally consistent and within the supplied bound. */
[[nodiscard]] bool valid_array(const HavokArray& array, std::int32_t maximum) noexcept {
    const std::uint32_t capacity = array.capacityAndFlags & kArrayCapacityMask;
    return array.size >= 0 && array.size <= maximum
           && static_cast<std::uint32_t>(array.size) <= capacity
           && (array.size == 0 || array.entries != nullptr);
}

/** @return True when one island holds the body. */
[[nodiscard]] bool island_holds(std::byte* island, const std::byte* body) noexcept {
    if (island == nullptr) {
        return false;
    }
    const HavokArray& entities = field<HavokArray>(island, kIslandEntities);
    if (!valid_array(entities, kMaximumEntityCount)) {
        return false;
    }
    for (std::int32_t index = 0; index < entities.size; ++index) {
        if (entities.entries[index] == body) {
            g_playerIslandSize = entities.size;
            return true;
        }
    }
    return false;
}

/** @return The length of a vector's three lanes. */
[[nodiscard]] float length_of(const std::array<float, kVectorLanes>& vector) noexcept {
    return std::sqrt(vector[kHorizontalX] * vector[kHorizontalX]
                     + vector[kHorizontalY] * vector[kHorizontalY]
                     + vector[kVertical] * vector[kVertical]);
}

/**
 * Adds one step to the fly measurement and logs the window once it is full.
 * @param found True when this step held the player's body.
 * @param noclip True when noclip also runs on this step.
 * @param deltaTime Step length in seconds.
 * @param asked Speed fly wrote before the step.
 * @param moved Distance the body moved in the step.
 * @param leftOver Speed left in the body after the step, before the cap.
 * @param position Body position before the step.
 */
void measure_fly(bool found,
                 bool noclip,
                 float deltaTime,
                 float asked,
                 float moved,
                 float leftOver,
                 const std::array<float, kVectorLanes>& position) noexcept {
    const ULONGLONG now = GetTickCount64();
    if (g_measure.windowStart == 0) {
        g_measure.windowStart = now;
    }
    ++g_measure.steps;
    g_measure.noclip = g_measure.noclip || noclip;
    if (found) {
        ++g_measure.found;
        g_measure.simulated += deltaTime;
        g_measure.asked += asked * deltaTime;
        g_measure.moved += moved;
        g_measure.leftOver += leftOver * deltaTime;
        g_measure.islandMax = (std::max)(g_measure.islandMax, g_playerIslandSize);
        g_measure.position = position;
    }
    const ULONGLONG wall = now - g_measure.windowStart;
    if (wall < kMeasureWindowMs) {
        return;
    }
    // Speeds are averaged over simulated time, so asked, moved and left compare directly.
    const float time = g_measure.simulated > 0.0F ? g_measure.simulated : 1.0F;
    std::array<char, 256> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=fly stage=measure steps=%u found=%u wall_ms=%llu "
        "sim_ms=%.0f asked=%.2f moved=%.2f left=%.2f island=%d noclip=%d at=%.1f,%.1f,%.1f",
        g_measure.steps,
        g_measure.found,
        static_cast<unsigned long long>(wall),
        static_cast<double>(g_measure.simulated * 1000.0F),
        static_cast<double>(g_measure.asked / time),
        static_cast<double>(g_measure.moved / time),
        static_cast<double>(g_measure.leftOver / time),
        g_measure.islandMax,
        g_measure.noclip ? 1 : 0,
        static_cast<double>(g_measure.position[0]),
        static_cast<double>(g_measure.position[1]),
        static_cast<double>(g_measure.position[2]));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    g_measure = FlyMeasure{};
    g_measure.windowStart = now;
}

/**
 * Finds the local player's rigid body in this simulation. Enemies share the character motion
 * type, so only the body of the player's own physics component is taken.
 * @param simulation Simulation being stepped.
 * @return The player's body, or null when this simulation does not hold it.
 */
[[nodiscard]] std::byte* player_body(std::byte* simulation) noexcept {
    if (simulation == nullptr) {
        return nullptr;
    }
    // The cached component may be stale. The pointer is only used once an island holds it.
    std::byte* const target =
        static_cast<std::byte*>(teleport::body(client::player::position::component()));
    std::byte* const world = field<std::byte*>(simulation, kSimulationWorld);
    if (target == nullptr || world == nullptr) {
        return nullptr;
    }
    for (const std::size_t offset : kWorldIslandArrays) {
        const HavokArray& islands = field<HavokArray>(world, offset);
        if (!valid_array(islands, kMaximumIslandCount)) {
            continue;
        }
        for (std::int32_t index = 0; index < islands.size; ++index) {
            if (island_holds(islands.entries[index], target)) {
                return target;
            }
        }
    }
    return nullptr;
}

/**
 * Polls the bound key on the physics thread and flips the stored switch when it goes down.
 * The key and the interface toggle write the same stored value, so there is one on/off state.
 * @return True while noclip is on.
 */
[[nodiscard]] bool poll_toggle() noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (settings.noclipToggleKey == client::movement::kNoKey) {
        g_toggleDown.store(false, std::memory_order_relaxed);
        return settings.noclipEnabled;
    }
    const bool down =
        client::input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.noclipToggleKey)) & 0x8000) != 0;
    // An open interface owns the keyboard, so the bound key only tracks the press, it never flips.
    if (core::ui::runtime::snapshot().visible) {
        g_toggleDown.store(down, std::memory_order_relaxed);
        return settings.noclipEnabled;
    }
    if (down && !g_toggleDown.exchange(true, std::memory_order_acq_rel)) {
        client::movement::Settings updated = settings;
        updated.noclipEnabled = !settings.noclipEnabled;
        if (!client::movement::publish(updated)) {
            return settings.noclipEnabled;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         updated.noclipEnabled
                             ? "ev=noclip stage=toggle enabled=1 mode=rigid_body_position"
                             : "ev=noclip stage=toggle enabled=0 mode=rigid_body_position");
        return updated.noclipEnabled;
    }
    if (!down) {
        g_toggleDown.store(false, std::memory_order_release);
    }
    return settings.noclipEnabled;
}

/** @return True while the stored switch has noclip on. */
[[nodiscard]] bool enabled() noexcept {
    return client::movement::get().noclipEnabled;
}

/** Runs Havok normally, then moves the character on from where it stood before the step. */
std::int32_t __fastcall havok_step(std::byte* simulation, float deltaTime) noexcept {
    std::array<float, kVectorLanes> nativeVelocity{};
    std::array<float, kVectorLanes> nativePosition{};
    const bool enabledBeforeStep = poll_toggle();
    const bool flying = fly::enabled();
    std::byte* const before = (enabledBeforeStep || flying) ? player_body(simulation) : nullptr;
    // Fly writes first, so the velocity read below is the one it asked for.
    if (flying) {
        fly::before_step(before);
    }
    const bool hasBody = before != nullptr;
    if (hasBody) {
        nativeVelocity = field<std::array<float, kVectorLanes>>(before, kBodyVelocity);
        nativePosition = field<std::array<float, kVectorLanes>>(before, kBodyPosition);
    }
    // Rest the body through the step: this hook writes the position, and contact damage scales
    // with speed. The held velocity goes back after the step.
    const bool rested = enabledBeforeStep && flying && hasBody;
    if (rested) {
        field<std::array<float, kVectorLanes>>(before, kBodyVelocity) = {};
    }

    const HavokStep next = reinterpret_cast<HavokStep>(g_stepHandle.original);
    const std::int32_t result = next != nullptr ? next(simulation, deltaTime) : 0;

    // The body is resolved once here for both features.
    std::byte* const body = (enabledBeforeStep || flying) ? player_body(simulation) : nullptr;
    // A character created or replaced during this step has no matching before-state.
    const bool sameBody = hasBody && body == before;
    // Re-read after the step, so a toggle from the interface thread lands before a position write.
    const bool noclipping = enabledBeforeStep && enabled();
    if (flying) {
        // Taken before any write below, so it shows what the step alone did to the body.
        std::array<float, kVectorLanes> moved{};
        std::array<float, kVectorLanes> leftOver{};
        if (sameBody) {
            moved = field<std::array<float, kVectorLanes>>(body, kBodyPosition);
            for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
                moved[lane] -= nativePosition[lane];
            }
            leftOver = field<std::array<float, kVectorLanes>>(body, kBodyVelocity);
        }
        measure_fly(sameBody,
                    noclipping,
                    deltaTime,
                    length_of(nativeVelocity),
                    length_of(moved),
                    length_of(leftOver),
                    nativePosition);
    }
    // With both on this hook drives all three lanes. Fly holds the height, so carrying the
    // vertical one is safe.
    const bool verticalToo = noclipping && flying;
    if (flying) {
        fly::after_step(body, verticalToo);
    }
    // The game reads this field after the step and damages the player for carrying speed into
    // geometry. It is shown a capped speed instead. Nothing is lost: the step it belonged to has
    // already run, and fly writes the real speed again before the next one.
    if (flying && sameBody) {
        const std::array<float, kVectorLanes> moved =
            rested ? nativeVelocity : field<std::array<float, kVectorLanes>>(body, kBodyVelocity);
        field<std::array<float, kVectorLanes>>(body, kBodyVelocity) =
            capped_speed(moved, fly::kPublishedSpeedCap);
    }
    if (!noclipping || !sameBody) {
        return result;
    }
    const float step = std::clamp(deltaTime, 0.0F, kMaximumStepSeconds);
    float velocitySquared = nativeVelocity[kHorizontalX] * nativeVelocity[kHorizontalX]
                            + nativeVelocity[kHorizontalY] * nativeVelocity[kHorizontalY];
    if (verticalToo) {
        // Straight up has no horizontal velocity, and without this the move is dropped.
        velocitySquared += nativeVelocity[kVertical] * nativeVelocity[kVertical];
    }
    if (step <= 0.0F || velocitySquared <= kMinimumVelocitySquared) {
        return result;
    }
    // Moved on from where the body stood before the step, not from a position of our own. The game
    // owns the position, so a respawn or any other placement is picked up with nothing to reset.
    std::array<float, kVectorLanes> position =
        field<std::array<float, kVectorLanes>>(body, kBodyPosition);
    position[kHorizontalX] = nativePosition[kHorizontalX] + nativeVelocity[kHorizontalX] * step;
    position[kHorizontalY] = nativePosition[kHorizontalY] + nativeVelocity[kHorizontalY] * step;
    if (verticalToo) {
        position[kVertical] = nativePosition[kVertical] + nativeVelocity[kVertical] * step;
    }
    field<std::array<float, kVectorLanes>>(body, kBodyPosition) = position;

    // Collision may consume velocity before publication. Restore it so the next step still moves.
    // The vertical lane stays resolved. A rested body already had every lane put back above.
    if (!rested) {
        std::array<float, kVectorLanes> wakeVelocity =
            field<std::array<float, kVectorLanes>>(body, kBodyVelocity);
        wakeVelocity[kHorizontalX] = nativeVelocity[kHorizontalX];
        wakeVelocity[kHorizontalY] = nativeVelocity[kHorizontalY];
        field<std::array<float, kVectorLanes>>(body, kBodyVelocity) = wakeVelocity;
    }
    return result;
}

/** @param reason Stable diagnostic key for an installation failure. */
void report_install_failure(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=noclip stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Resolves the Havok targets and attaches the simulation-step detour. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const step = patterns::scan_main_image_unique(kHavokStep, "noclip_havok_step");
    if (step == nullptr) {
        report_install_failure("havok_step");
        return false;
    }
    if (!hooking::detour::install(hooking::detour::Spec{step, reinterpret_cast<void*>(&havok_step)},
                                  g_stepHandle)) {
        report_install_failure("attach");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=noclip stage=install result=ok");
    return true;
}

/** Detaches the simulation-step detour, then clears the key state. */
void uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    // A failed detach keeps the handle, so the detour still reaches the original.
    if (!hooking::detour::uninstall(g_stepHandle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=noclip stage=uninstall result=fail");
        return;
    }
    g_installed.store(false, std::memory_order_release);
    // The switch is a stored setting, so detaching clears only the key state.
    g_toggleDown.store(false, std::memory_order_release);
}

/** Reads a live rigid body's world position. */
void read_body_position(void* body, Vector& position) noexcept {
    copy_lanes(field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyPosition),
               position);
}

/** Writes a live rigid body's world position. */
void write_body_position(void* body, const Vector& position) noexcept {
    auto& stored =
        field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyPosition);
    copy_lanes(position, stored);
}

/** Reads a live rigid body's linear velocity. */
void read_body_velocity(void* body, Vector& velocity) noexcept {
    copy_lanes(field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyVelocity),
               velocity);
}

/** Writes a live rigid body's linear velocity. */
void write_body_velocity(void* body, const Vector& velocity) noexcept {
    auto& stored =
        field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyVelocity);
    copy_lanes(velocity, stored);
}

} // namespace sunrise::client::hooks::noclip
