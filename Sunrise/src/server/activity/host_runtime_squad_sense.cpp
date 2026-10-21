#include <algorithm>
#include <new>

#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace detail {
/** Merges complete squad deltas without discarding fields absent from a later report. */
void retain_squad_sense(Instance& instance, const SenseInput& input) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    namespace squadSense = middleware::bap::activity_message::squad_sense;
    const sense::DecodedPacket& packet = input.decoded;
    if (input.sourceGeneration == 0 || packet.status == sense::DecodeStatus::malformed
        || packet.valuesTruncated || packet.objectsTruncated
        || packet.objectCount > packet.objects.size() || packet.valueCount > packet.values.size()) {
        return;
    }
    for (const sense::DecodedObject& object : std::span(packet.objects).first(packet.objectCount)) {
        if (object.slotType != squadSense::kSlotType || object.senseSchema != squadSense::kSchema
            || object.status != sense::ObjectStatus::decoded || !object.hasGeneration) {
            continue;
        }
        auto found = std::ranges::find_if(instance.squadSense, [&](const SquadSenseRecord& record) {
            return record.sourceGeneration == input.sourceGeneration
                   && (record.key.registryKey == object.registryKey
                       && record.key.objectTag == object.objectTag
                       && record.key.senseSchema == object.senseSchema
                       && record.key.schemaRow == object.schemaRow
                       && record.key.slotType == object.slotType
                       && record.key.slotIndex == object.slotIndex);
        });
        squadSense::State merged =
            found == instance.squadSense.end() ? squadSense::State{} : found->state;
        if (!squadSense::merge(merged, object, std::span(packet.values).first(packet.valueCount))) {
            continue;
        }
        // The owning client can withdraw its initialized state. That invalidation must not
        // leave an old recovery body publishable, and never erases a different member's body.
        if (!merged.valid) {
            if (found != instance.squadSense.end()) {
                instance.squadSense.erase(found);
            }
            continue;
        }
        if (found != instance.squadSense.end()) {
            found->state = merged;
        } else if (instance.squadSense.size()
                   < kScriptableGuardCapacity
                         * state::activity::entity_slots::kMemberLeaseRowCount) {
            const SenseObservationKey key{object.registryKey,
                                          object.objectTag,
                                          object.senseSchema,
                                          object.schemaRow,
                                          object.slotIndex,
                                          object.slotType};
            try {
                instance.squadSense.push_back({key, merged, input.sourceGeneration});
            } catch (const std::bad_alloc&) {
                return;
            }
        }
    }
}

} // namespace detail
using namespace detail;

void retain_squad_sense_generations(const state::activity::SessionBinding& binding,
                                    std::span<const std::uint64_t> generations) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (auto* instance = find_instance(binding)) {
        std::erase_if(instance->squadSense, [&](const auto& record) {
            return std::find(generations.begin(), generations.end(), record.sourceGeneration)
                   == generations.end();
        });
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies initialized recovery state for one exact ActivityClient generation. */
bool snapshot_squad_sense(const state::activity::SessionBinding& binding,
                          std::uint64_t sourceGeneration,
                          const SenseObservationKey& key,
                          middleware::bap::activity_message::squad_sense::State& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    bool found = false;
    if (instance != nullptr && sourceGeneration != 0) {
        for (const SquadSenseRecord& record : instance->squadSense) {
            if (record.sourceGeneration == sourceGeneration
                && record.key.registryKey == key.registryKey
                && record.key.objectTag == key.objectTag
                && record.key.senseSchema == key.senseSchema && record.key.slotType == key.slotType
                && record.key.slotIndex == key.slotIndex && record.state.valid) {
                output = record.state;
                found = true;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

} // namespace sunrise::server::activity::host
