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
        // A withdrawn record, or one from an earlier attempt, holds an old state; the source's
        // next report starts a new one.
        squadSense::State merged = found == instance.squadSense.end() || found->withdrawn
                                           || found->attemptGeneration != input.attemptGeneration
                                       ? squadSense::State{}
                                       : found->state;
        if (!squadSense::merge(merged, object, std::span(packet.values).first(packet.valueCount))) {
            continue;
        }
        // The owning client can withdraw its initialized state. That invalidation must not leave
        // an old recovery body publishable to that client, and never touches a different member's
        // body. The last valid state stays for the link that takes over the squad's bubble.
        if (!merged.valid) {
            if (found != instance.squadSense.end()) {
                found->withdrawn = true;
            }
            continue;
        }
        if (found != instance.squadSense.end()) {
            found->state = merged;
            found->withdrawn = false;
            found->attemptGeneration = input.attemptGeneration;
            found->mergeSequence = ++instance.squadSenseSequence;
            found->heldBubbles = input.heldBubbles;
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
                instance.squadSense.push_back({key,
                                               merged,
                                               input.sourceGeneration,
                                               input.attemptGeneration,
                                               ++instance.squadSenseSequence,
                                               input.heldBubbles,
                                               false});
            } catch (const std::bad_alloc&) {
                return;
            }
        }
    }
}

} // namespace detail
using namespace detail;

void retain_squad_sense_generations(const state::activity::SessionBinding& binding,
                                    std::span<const std::uint64_t> generations,
                                    std::uint64_t attemptGeneration) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (auto* instance = find_instance(binding)) {
        std::erase_if(instance->squadSense, [&](const auto& record) {
            return record.attemptGeneration != attemptGeneration
                   || std::find(generations.begin(), generations.end(), record.sourceGeneration)
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
                && record.key.slotIndex == key.slotIndex && record.state.valid
                && !record.withdrawn) {
                output = record.state;
                found = true;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

namespace {

[[nodiscard]] bool same_squad(const SenseObservationKey& left,
                              const SenseObservationKey& right) noexcept {
    return left.registryKey == right.registryKey && left.objectTag == right.objectTag
           && left.senseSchema == right.senseSchema && left.slotType == right.slotType
           && left.slotIndex == right.slotIndex;
}

template <typename Records>
[[nodiscard]] auto* find_record(Records& records,
                                std::uint64_t sourceGeneration,
                                const SenseObservationKey& key) noexcept {
    decltype(&records.front()) found = nullptr;
    for (auto& record : records) {
        if (record.sourceGeneration == sourceGeneration && same_squad(record.key, key)) {
            found = &record;
            break;
        }
    }
    return found;
}

/** Root scalar 3 is the squad's alive count; its created-count list says members were placed. */
constexpr std::size_t kAliveScalar = 3;

/** @return True when the squad placed members and none is alive: a cleared encounter. */
[[nodiscard]] bool
cleared(const middleware::bap::activity_message::squad_sense::State& state) noexcept {
    const auto& alive = state.scalars[kAliveScalar];
    if (!alive.present || alive.raw != 0 || !state.countsPresent) {
        return false;
    }
    const std::size_t length =
        (std::min)(static_cast<std::size_t>(state.count), state.counts.size());
    return std::any_of(state.counts.begin(),
                       state.counts.begin() + static_cast<std::ptrdiff_t>(length),
                       [](std::int32_t created) { return created > 0; });
}

/** @return True when the record's state was reported while its source held this bubble. */
[[nodiscard]] bool simulated(const SquadSenseRecord& record, std::uint8_t bubble) noexcept {
    return bubble < state::build_data::scenarios::kBubbleCapacity
           && (record.heldBubbles & (std::uint64_t{1} << bubble)) != 0;
}

/**
 * The bubble's truth for one squad is its releaser's last valid state, withdrawn or not: the
 * releaser held the bubble, so it simulated the squad. The recipient's own record replaces it only
 * when it is newer and was reported while the recipient held the bubble, as when a client walks
 * back into an encounter it cleared. A newer report from the recipient's replica does not: arrival
 * order says nothing about who simulated the squad. With no valid releaser state, the recipient's
 * own record is the only one. The recipient is owed the truth unless it is its own live record,
 * which every body replays. A recipient that unloaded the squad, by withdrawing it or by releasing
 * the bubble itself, reloads it: a cleared encounter stays cleared for it, and any other squad
 * starts natively, since its members went with the unload.
 * @param bubble The bubble being granted, which the squad is authored in.
 * @return The record whose state the recipient is owed, or null.
 */
[[nodiscard]] const SquadSenseRecord* handover_owed(const SquadSenseRecord* released,
                                                    const SquadSenseRecord* own,
                                                    std::uint8_t bubble) noexcept {
    const SquadSenseRecord* truth =
        released != nullptr && released->state.valid ? released : nullptr;
    if (own != nullptr && own->state.valid
        && (truth == nullptr
            || (simulated(*own, bubble) && own->mergeSequence > truth->mergeSequence))) {
        truth = own;
    }
    if (truth == nullptr || (truth == own && !own->withdrawn)) {
        return nullptr;
    }
    const bool reloads = own != nullptr && (own->withdrawn || own == released);
    return reloads && !cleared(truth->state) ? nullptr : truth;
}

} // namespace

std::size_t handover_squad_sense_keys(const state::activity::SessionBinding& binding,
                                      std::uint64_t recipientGeneration,
                                      std::uint64_t releaserGeneration,
                                      std::span<SenseObservationKey> output) noexcept {
    std::size_t count = 0;
    AcquireSRWLockShared(&g_lock);
    if (const Instance* const instance = find_instance(binding);
        instance != nullptr && recipientGeneration != 0 && releaserGeneration != 0) {
        for (const SquadSenseRecord& record : instance->squadSense) {
            if (count == output.size()) {
                break;
            }
            const bool source = record.sourceGeneration == releaserGeneration
                                || record.sourceGeneration == recipientGeneration;
            const auto listed = output.first(count);
            if (source && record.state.valid
                && std::none_of(listed.begin(), listed.end(), [&](const auto& key) {
                       return same_squad(key, record.key);
                   })) {
                output[count++] = record.key;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return count;
}

/**
 * The releaser's last valid state is the bubble's truth even though it withdrew it at the
 * crossing that released the bubble, and so is the recipient's own withdrawn state when it is the
 * newer one and the recipient held the bubble then: the recipient walks back into an encounter it
 * cleared.
 */
bool snapshot_handover_squad_sense(
    const state::activity::SessionBinding& binding,
    std::uint64_t recipientGeneration,
    std::uint64_t releaserGeneration,
    std::uint8_t bubble,
    const SenseObservationKey& key,
    middleware::bap::activity_message::squad_sense::State& output) noexcept {
    output = {};
    const SquadSenseRecord* owed = nullptr;
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    if (instance != nullptr && recipientGeneration != 0 && releaserGeneration != 0) {
        const SquadSenseRecord* const released =
            find_record(instance->squadSense, releaserGeneration, key);
        const SquadSenseRecord* const own =
            find_record(instance->squadSense, recipientGeneration, key);
        owed = handover_owed(released, own, bubble);
        if (owed != nullptr) {
            output = owed->state;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return owed != nullptr;
}

std::size_t seed_handover_squad_sense(const state::activity::SessionBinding& binding,
                                      std::uint64_t recipientGeneration,
                                      std::uint64_t releaserGeneration,
                                      std::uint8_t bubble,
                                      std::span<const SenseObservationKey> keys) noexcept {
    std::size_t seeded = 0;
    if (recipientGeneration == 0 || releaserGeneration == 0) {
        return seeded;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (Instance* const instance = find_instance(binding)) {
        for (const SenseObservationKey& key : keys) {
            const SquadSenseRecord* const released =
                find_record(instance->squadSense, releaserGeneration, key);
            SquadSenseRecord* const own =
                find_record(instance->squadSense, recipientGeneration, key);
            const SquadSenseRecord* const owed = handover_owed(released, own, bubble);
            if (owed == nullptr) {
                continue;
            }
            SquadSenseRecord seed = *owed;
            seed.sourceGeneration = recipientGeneration;
            seed.withdrawn = false;
            seed.mergeSequence = ++instance->squadSenseSequence;
            // The body that delivered it grants the recipient the bubble, so it simulates the
            // squad from this state on.
            seed.heldBubbles = bubble < state::build_data::scenarios::kBubbleCapacity
                                   ? std::uint64_t{1} << bubble
                                   : 0;
            if (own != nullptr) {
                *own = seed;
            } else if (instance->squadSense.size()
                       < kScriptableGuardCapacity
                             * state::activity::entity_slots::kMemberLeaseRowCount) {
                try {
                    instance->squadSense.push_back(seed);
                } catch (const std::bad_alloc&) {
                    break;
                }
            } else {
                continue;
            }
            ++seeded;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return seeded;
}

} // namespace sunrise::server::activity::host
