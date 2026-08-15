#include "ui_transition_animation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <imgui.h>

namespace sunrise::core::ui::animation::transition {
namespace {

/** 256 lanes cover 128 two-state controls with no heap storage. */
constexpr std::size_t kTransitionCapacity = 256;
/** A 0.1 second cap stops a stalled frame from skipping a whole transition. */
constexpr float kMaximumDeltaSeconds = 0.1F;
/** Values within one alpha step snap, so idle slots settle exactly on a target. */
constexpr float kSnapDistance = 1.0F / 255.0F;
/** Lower end of the normalized transition range. */
constexpr float kMinimumValue = 0.0F;
/** Upper end of the normalized transition range. */
constexpr float kMaximumValue = 1.0F;

/** One fixed-table lane. Holds the normalized value and the frames used for eviction. */
struct Slot {
    ImGuiID id{};
    Lane lane{};
    float value{};
    std::uint32_t lastUpdateFrame{};
    std::uint32_t lastSeenFrame{};
    bool occupied{};
};

std::array<Slot, kTransitionCapacity> g_slots{};

/** @return The value clamped into the normalized animation range. */
[[nodiscard]] float normalize(float value) noexcept {
    return (std::clamp)(value, kMinimumValue, kMaximumValue);
}

/**
 * Finds one lane, or replaces the lane unused for the most frames.
 * @param id Stable Dear ImGui widget ID.
 * @param frame Current Dear ImGui frame number.
 * @param initialValue Normalized value given to a new lane.
 * @return Fixed-table slot owned by the requested key.
 */
[[nodiscard]] Slot&
find_or_claim(ImGuiID id, Lane lane, std::uint32_t frame, float initialValue) noexcept {
    Slot* available = nullptr;
    Slot* oldest = &g_slots.front();
    std::uint32_t oldestAge = 0;

    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.id == id && slot.lane == lane) {
            return slot;
        }
        if (!slot.occupied && available == nullptr) {
            available = &slot;
            continue;
        }
        if (slot.occupied) {
            const std::uint32_t age = frame - slot.lastSeenFrame;
            if (age > oldestAge) {
                oldest = &slot;
                oldestAge = age;
            }
        }
    }

    // A full table reuses the oldest lane, so memory stays fixed in long sessions.
    Slot& claimed = available != nullptr ? *available : *oldest;
    claimed = {id, lane, normalize(initialValue), frame, frame, true};
    return claimed;
}

/**
 * Moves a normalized value toward its target, taking the same time at any frame rate.
 * @param rate Positive response rate per second.
 * @param deltaSeconds Frame duration, already capped.
 * @return The advanced normalized value.
 */
[[nodiscard]] float advance(float value, float target, float rate, float deltaSeconds) noexcept {
    // Exponential decay gives the same duration at any frame rate. A plain rate times delta hits
    // a blend of 1 below 17 frames per second, which skips the transition instead of playing it.
    const float blend = 1.0F - std::exp(-(std::max)(rate, kMinimumValue) * deltaSeconds);
    const float advanced = value + ((target - value) * blend);
    if (std::fabs(target - advanced) <= kSnapDistance) {
        return target;
    }
    return advanced;
}

} // namespace

/** Advances one bounded transition during the current Dear ImGui frame. */
float update(ImGuiID id, Lane lane, bool enabled, Rates rates, float initialValue) noexcept {
    const float target = enabled ? kMaximumValue : kMinimumValue;
    if (ImGui::GetCurrentContext() == nullptr) {
        return target;
    }

    const std::uint32_t frame = static_cast<std::uint32_t>(ImGui::GetFrameCount());
    Slot& slot = find_or_claim(id, lane, frame, initialValue);
    slot.lastSeenFrame = frame;
    if (slot.lastUpdateFrame == frame) {
        return slot.value;
    }

    const float deltaSeconds =
        (std::clamp)(ImGui::GetIO().DeltaTime, kMinimumValue, kMaximumDeltaSeconds);
    const float rate = target > slot.value ? rates.risePerSecond : rates.fallPerSecond;
    slot.value = advance(slot.value, target, rate, deltaSeconds);
    slot.lastUpdateFrame = frame;
    return slot.value;
}

/** Clears every transition after all active UI render calls have stopped. */
void reset() noexcept {
    g_slots = {};
}

} // namespace sunrise::core::ui::animation::transition
