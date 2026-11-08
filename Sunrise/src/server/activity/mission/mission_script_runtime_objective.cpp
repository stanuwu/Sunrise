#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

/** The block's ordinal 1 is its one-bit client flag, which separates it from the task list. */
constexpr std::uint16_t kObjectiveBlockFlagOrdinal = 1;
constexpr std::uint8_t kObjectiveBlockFlagWidth = 1;
/** The client clamps a task counter here, so nothing above it is reachable. */
constexpr std::int32_t kObjectiveTaskCountLimit = 80;

namespace sense_values = middleware::bap::activity_message::sense_update;

/** @return The retained record for one objective sensor, allocating on a first observation. */
[[nodiscard]] ObjectiveObservation* find_objective(RuntimeInstance& instance,
                                                   const host::SenseObservationKey& key) noexcept {
    ObjectiveObservation* spare = nullptr;
    for (ObjectiveObservation& retained : instance.objectiveObservations) {
        if (retained.used && retained.registryKey == key.registryKey
            && retained.objectTag == key.objectTag && retained.slotIndex == key.slotIndex) {
            return &retained;
        }
        if (!retained.used && spare == nullptr) {
            spare = &retained;
        }
    }
    if (spare == nullptr) {
        log_line(core::log::Level::warn, &instance, "objective", "watch_capacity");
        return nullptr;
    }
    spare->registryKey = key.registryKey;
    spare->objectTag = key.objectTag;
    spare->slotIndex = key.slotIndex;
    return spare;
}

/** One objective sensor's task counters and the exact set the body carried. */
struct ObjectiveCounters final {
    std::array<std::array<std::uint8_t, kObjectiveTaskCapacity>, kObjectiveCapacity> value{};
    std::array<std::array<bool, kObjectiveTaskCapacity>, kObjectiveCapacity> present{};
    std::uint8_t blocks{};
};

/**
 * Copies one objective sensor's task counters out of its two nested schemas.
 * The 24 blocks share one schema row, so their order is the only separator: a block opens with its
 * own ordinal 0, then its task list. An omitted counter is absent, because zero is a real value.
 */
void objective_task_counters(std::span<const sense_values::DecodedValue> body,
                             std::uint32_t rootSchemaRow,
                             ObjectiveCounters& output) noexcept {
    output = {};
    std::uint32_t blockSchemaRow = sense_values::kAbsentRuntimeRow;
    for (const sense_values::DecodedValue& value : body) {
        if (value.schemaRow != rootSchemaRow && value.fieldOrdinal == kObjectiveBlockFlagOrdinal
            && value.width == kObjectiveBlockFlagWidth) {
            blockSchemaRow = value.schemaRow;
            break;
        }
    }
    if (blockSchemaRow == sense_values::kAbsentRuntimeRow) {
        return;
    }
    std::size_t blocks = 0;
    std::size_t tasks = 0;
    for (const sense_values::DecodedValue& value : body) {
        if (value.schemaRow == rootSchemaRow) {
            continue;
        }
        if (value.schemaRow == blockSchemaRow) {
            if (value.fieldOrdinal == 0) {
                ++blocks;
                tasks = 0;
            }
            continue;
        }
        if (blocks == 0 || blocks > output.value.size() || tasks >= kObjectiveTaskCapacity) {
            continue;
        }
        if (value.present) {
            const std::int64_t raw = value.signedValue;
            const std::int64_t clamped =
                raw <= 0 ? 0 : (std::min)(raw, std::int64_t{kObjectiveTaskCountLimit});
            output.value[blocks - 1][tasks] = static_cast<std::uint8_t>(clamped);
            output.present[blocks - 1][tasks] = true;
        }
        ++tasks;
    }
    output.blocks = static_cast<std::uint8_t>((std::min)(blocks, output.value.size()));
}

} // namespace

/**
 * Raises one event per watched objective task counter that rose.
 * The client owns the counter and raises it on an actor teardown, so a rise is the only progress
 * signal. A sensor seen for the first time records its counters and raises nothing.
 */
void push_objective_edges(RuntimeInstance& instance,
                          const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != format::kObjectiveSlotType
            || observation.key.senseSchema != format::kObjectiveSenseSchema
            || observation.firstValue + observation.valueCount > sense.valueCount) {
            continue;
        }
        const std::span<const sense_values::DecodedValue> body(
            &sense.values[observation.firstValue], observation.valueCount);
        ObjectiveCounters counters{};
        objective_task_counters(body, observation.key.schemaRow, counters);
        // An unchanged sensor sends an empty delta, which must not read as every counter at zero.
        if (counters.blocks == 0) {
            continue;
        }
        ObjectiveObservation* const watched = find_objective(instance, observation.key);
        if (watched == nullptr) {
            continue;
        }
        const bool first = !watched->used;
        watched->used = true;
        for (std::uint8_t block = 0; block < counters.blocks; ++block) {
            for (std::size_t task = 0; task < kObjectiveTaskCapacity; ++task) {
                if (!counters.present[block][task]) {
                    continue;
                }
                const std::uint8_t value = counters.value[block][task];
                const std::uint8_t previous = watched->counters[block][task];
                watched->counters[block][task] = value;
                if (first || value <= previous) {
                    continue;
                }
                host::Event event = sense_edge_event(instance, observation);
                event.objectiveOrdinal = block;
                event.objectiveTaskOrdinal = static_cast<std::uint8_t>(task);
                event.objectiveTaskCount = value;
                event.objectivePreviousTaskCount = previous;
                event.kind = host::EventKind::objectiveProgress;
                push_script_event(instance, event);
            }
        }
    }
}

} // namespace sunrise::server::activity::mission
