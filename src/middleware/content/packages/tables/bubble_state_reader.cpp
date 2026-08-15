#include "bubble_state_reader.h"

#include <algorithm>

#include "component_container_reader.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/**
 * Adds one package id to the distinct list.
 * @param output Scenario package list.
 * @param packageId Candidate id.
 */
void add_package(BubbleStates& output, std::uint16_t packageId) noexcept {
    if (packageId == kAbsentPackageId || output.packageCount >= output.packages.size()) {
        return;
    }
    for (std::size_t index = 0; index < output.packageCount; ++index) {
        if (output.packages[index] == packageId) {
            return;
        }
    }
    output.packages[output.packageCount++] = packageId;
}

} // namespace

/** Builds one scenario's per-bubble state array. */
bool bubble_states(std::span<const std::byte> scenario, BubbleStates& output) noexcept {
    output = {};
    Array bubbles{};
    if (!scenario_bubbles(scenario, bubbles)) {
        return false;
    }
    output.truncated = bubbles.count > kBubbleStateCapacity;
    const std::uint64_t reported = output.truncated ? kBubbleStateCapacity : bubbles.count;

    for (std::uint64_t index = 0; index < reported; ++index) {
        Bubble bubble{};
        if (!bubble_at(scenario, bubbles, index, bubble)) {
            output = {};
            return false;
        }
        // A bubble with no readable state array is disabled, the same as a cleared first state.
        std::uint8_t value = kBubbleDisabledByte;
        SliceState state{};
        std::uint16_t mapIndex = kAbsentMapBubbleIndex;
        if (bubble.stateCount != 0 && slice_state_at(scenario, bubble, 0, state)) {
            value = state.enabled ? kBubbleEnabledByte : kBubbleDisabledByte;
            // An index no container mask can name is absent, because nothing could match it.
            if (state.mapBubbleIndex < kBubbleIndexCapacity) {
                mapIndex = static_cast<std::uint16_t>(state.mapBubbleIndex);
            }
        }
        // Every state names its slice-set entry, and the entry's package is one this destination
        // loads. A bubble's later states can name a package its first one does not.
        for (std::uint64_t ordinal = 0; ordinal < bubble.stateCount; ++ordinal) {
            SliceState each{};
            if (slice_state_at(scenario, bubble, ordinal, each)) {
                add_package(output, package_of(each.entryTag));
            }
        }
        const std::uint64_t states = (std::min)(bubble.stateCount, kBubbleStateCountCeiling);
        output.bytes[static_cast<std::size_t>(index)] = value;
        output.hashes[static_cast<std::size_t>(index)] = bubble.nameHash;
        output.stateCounts[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(states);
        output.mapIndices[static_cast<std::size_t>(index)] = mapIndex;
    }
    output.count = static_cast<std::size_t>(reported);
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
