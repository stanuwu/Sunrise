#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "../../../middleware/bap/activity_message/auth_fields.h"
#include "../../../middleware/encoding/bit_reader.h"
#include "../../../state/activity_sdk/format.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_trigger_volume.h"

// Type-30 player monitors evaluated by the host from the positions clients report on channel 3.
// A client in a private activity never counts players in these monitors itself: its owner filter
// compares each player to the Activity Host it joined, so the host is the one that must measure.

namespace sunrise::server::activity::mission {
namespace {

namespace catalog = state::build_data::scriptables;

/** A player monitor measures the type-60 trigger volume its descriptor names. */
constexpr std::uint16_t kVolumeSlotType = 60;
/** A client sends the origin until its player is placed; that is not a position. */
constexpr float kUnplacedPosition = 0.0F;

/** @return True when one catalog slot is an exact type-30 player monitor. */
[[nodiscard]] bool exact_monitor_slot(const catalog::Snapshot& world,
                                      const catalog::Slot& slot) noexcept {
    if (slot.slotType != format::kOccupancySlotType || slot.descriptorCount == 0
        || slot.firstDescriptor > world.descriptors.size()
        || slot.descriptorCount > world.descriptors.size() - slot.firstDescriptor) {
        return false;
    }
    for (std::size_t row = slot.firstDescriptor; row < slot.firstDescriptor + slot.descriptorCount;
         ++row) {
        const catalog::Descriptor& descriptor = world.descriptors[row];
        if (descriptor.componentClass != format::kOccupancyComponentClass
            || descriptor.senseSchema != format::kOccupancySenseSchema) {
            return false;
        }
    }
    return true;
}

/** @return The cached monitor for one slot identity, or null. */
[[nodiscard]] HostMonitor* find_monitor(HostOccupancy& occupancy,
                                        std::uint32_t registryKey,
                                        std::uint32_t objectTag,
                                        std::uint16_t slotIndex) noexcept {
    for (std::size_t index = 0; index < occupancy.monitorCount; ++index) {
        HostMonitor& monitor = occupancy.monitors[index];
        if (monitor.registryKey == registryKey && monitor.objectTag == objectTag
            && monitor.slotIndex == slotIndex) {
            return &monitor;
        }
    }
    return nullptr;
}

/**
 * Collects every type-30 monitor the catalog joins to exactly one type-60 volume. A monitor
 * joined to more than one volume is ambiguous and left to the client.
 */
void resolve_monitors(RuntimeInstance& instance) noexcept {
    HostOccupancy& occupancy = instance.hostOccupancy;
    if (occupancy.resolved) {
        return;
    }
    const catalog::Snapshot* const world = instance.worldView.snapshot();
    if (world == nullptr) {
        return;
    }
    occupancy.resolved = true;
    occupancy.monitorCount = 0;
    std::array<bool, kHostMonitorCapacity> ambiguous{};
    std::size_t dropped = 0;
    for (const catalog::TriggerVolumeOwner& owner : world->triggerVolumeOwners) {
        if (owner.tableRow >= world->triggerVolumeTables.size()
            || world->triggerVolumeTables[owner.tableRow].slotType != kVolumeSlotType
            || owner.firstIncomingReference > world->triggerVolumeIncomingReferences.size()
            || owner.incomingReferenceCount
                   > world->triggerVolumeIncomingReferences.size() - owner.firstIncomingReference) {
            continue;
        }
        for (std::size_t row = owner.firstIncomingReference;
             row < owner.firstIncomingReference + owner.incomingReferenceCount;
             ++row) {
            const catalog::TriggerVolumeIncomingReference& incoming =
                world->triggerVolumeIncomingReferences[row];
            if (incoming.sourceObjectRow >= world->objects.size()
                || incoming.sourceSlotRow >= world->slots.size()) {
                continue;
            }
            const catalog::Object& object = world->objects[incoming.sourceObjectRow];
            const catalog::Slot& slot = world->slots[incoming.sourceSlotRow];
            if (slot.objectRow != incoming.sourceObjectRow || !exact_monitor_slot(*world, slot)) {
                continue;
            }
            if (HostMonitor* const known =
                    find_monitor(occupancy, object.registryKey, object.objectTag, slot.slotIndex);
                known != nullptr) {
                if (known->tableRow != owner.tableRow) {
                    ambiguous[static_cast<std::size_t>(known - occupancy.monitors.data())] = true;
                }
                continue;
            }
            if (occupancy.monitorCount == occupancy.monitors.size()) {
                ++dropped;
                continue;
            }
            occupancy.monitors[occupancy.monitorCount++] = {
                object.registryKey, object.objectTag, owner.tableRow, slot.slotIndex, 0, false};
        }
    }
    std::size_t kept = 0;
    for (std::size_t index = 0; index < occupancy.monitorCount; ++index) {
        if (!ambiguous[index]) {
            occupancy.monitors[kept++] = occupancy.monitors[index];
        }
    }
    const std::size_t ambiguousCount = occupancy.monitorCount - kept;
    occupancy.monitorCount = kept;
    std::array<char, 96> fields{};
    const int written = std::snprintf(fields.data(),
                                      fields.size(),
                                      "monitors=%zu ambiguous=%zu dropped=%zu",
                                      kept,
                                      ambiguousCount,
                                      dropped);
    log_line(dropped == 0 ? core::log::Level::info : core::log::Level::warn,
             &instance,
             "host_occupancy",
             "resolved",
             written > 0 ? std::string_view(
                               fields.data(),
                               (std::min)(static_cast<std::size_t>(written), fields.size() - 1))
                         : std::string_view{});
}

/** @return True when `point` lies inside any live instance of one type-60 table. */
[[nodiscard]] bool inside_table(const catalog::Snapshot& world,
                                std::uint32_t tableRow,
                                const std::array<float, 3>& point) noexcept {
    if (tableRow >= world.triggerVolumeTables.size()) {
        return false;
    }
    const catalog::TriggerVolumeTable& table = world.triggerVolumeTables[tableRow];
    if (table.firstInstance > world.triggerVolumeInstances.size()
        || table.instanceCount > world.triggerVolumeInstances.size() - table.firstInstance) {
        return false;
    }
    for (std::size_t row = table.firstInstance; row < table.firstInstance + table.instanceCount;
         ++row) {
        if (trigger_volume::contains(world, world.triggerVolumeInstances[row], point)) {
            return true;
        }
    }
    return false;
}

/** @return The retained row for one monitor, or null when no source has reported it yet. */
[[nodiscard]] TriggerOccupancy* existing_row(RuntimeInstance& instance,
                                             const HostMonitor& monitor) noexcept {
    for (TriggerOccupancy& row : instance.triggerOccupancy) {
        if (row.used && row.registryKey == monitor.registryKey && row.objectTag == monitor.objectTag
            && row.slotIndex == monitor.slotIndex) {
            return &row;
        }
    }
    return nullptr;
}

/** Raises the host's edge for one monitor whose combined level moved. */
void push_host_edge(RuntimeInstance& instance,
                    const HostMonitor& monitor,
                    const TriggerOccupancy& row,
                    std::int32_t count,
                    bool all,
                    std::uint64_t tick) noexcept {
    host::Event event{};
    event.kind = row.occupied ? host::EventKind::triggerEntered : host::EventKind::triggerExited;
    event.binding = instance.view.binding;
    event.sequence = instance.missionStateRevision;
    event.missionSequence = instance.lastMissionSequence;
    event.sourceGeneration = instance.view.activityClientGeneration;
    event.tick = tick;
    event.firstRegistryKey = monitor.registryKey;
    event.firstSlotType = static_cast<std::uint8_t>(format::kOccupancySlotType);
    event.firstSlotIndex = monitor.slotIndex;
    event.slotObjectTag = monitor.objectTag;
    event.slotSenseSchema = format::kOccupancySenseSchema;
    event.triggerCount = count;
    event.triggerValue = monitor.value;
    event.triggerAll = all;
    push_script_event(instance, event);
    std::array<char, 96> fields{};
    const int written = std::snprintf(fields.data(),
                                      fields.size(),
                                      "key=%08x object=%08x slot=%u count=%d",
                                      monitor.registryKey,
                                      monitor.objectTag,
                                      static_cast<unsigned>(monitor.slotIndex),
                                      count);
    log_line(core::log::Level::info,
             &instance,
             "host_occupancy",
             row.occupied ? "entered" : "exited",
             written > 0 ? std::string_view(
                               fields.data(),
                               (std::min)(static_cast<std::size_t>(written), fields.size() - 1))
                         : std::string_view{});
}

/** Re-measures every monitor against the current players and raises the edges that moved. */
void evaluate_monitors(RuntimeInstance& instance, std::uint64_t tick) noexcept {
    const catalog::Snapshot* const world = instance.worldView.snapshot();
    HostOccupancy& occupancy = instance.hostOccupancy;
    if (world == nullptr) {
        return;
    }
    std::int32_t present = 0;
    for (const HostPlayerPosition& player : occupancy.players) {
        present += player.used ? 1 : 0;
    }
    for (std::size_t index = 0; index < occupancy.monitorCount; ++index) {
        const HostMonitor& monitor = occupancy.monitors[index];
        if (monitor.filtered) {
            continue;
        }
        std::int32_t count = 0;
        for (const HostPlayerPosition& player : occupancy.players) {
            if (player.used && inside_table(*world, monitor.tableRow, player.position)) {
                ++count;
            }
        }
        // An empty level on a volume nobody has reported is only the baseline, so it takes no row;
        // the retained rows are shared with client Sense and are far fewer than the monitors.
        bool created = false;
        TriggerOccupancy* const row =
            count == 0
                ? existing_row(instance, monitor)
                : find_trigger_occupancy(
                      instance, monitor.registryKey, monitor.objectTag, monitor.slotIndex, created);
        if (row == nullptr) {
            continue;
        }
        // The host creates a row only once someone is inside, so its first level is an entry.
        row->hostOccupied = count != 0;
        if (settle_trigger_occupancy(*row, created, true)) {
            push_host_edge(instance, monitor, *row, count, count != 0 && count == present, tick);
        }
        // A host-only row that is empty again equals having none; free it for the next volume.
        if (!row->occupied && !row->clientReported) {
            *row = {};
        }
    }
}

} // namespace

void observe_host_player_position(RuntimeInstance& instance,
                                  std::uint64_t playerKey,
                                  const std::array<float, 3>& position,
                                  std::uint64_t tick) noexcept {
    if (instance.programStatus != ProgramStatus::loaded
        || std::any_of(
            position.begin(), position.end(), [](float axis) { return !std::isfinite(axis); })
        || std::all_of(position.begin(), position.end(), [](float axis) {
               return axis == kUnplacedPosition;
           })) {
        return;
    }
    resolve_monitors(instance);
    HostOccupancy& occupancy = instance.hostOccupancy;
    HostPlayerPosition* slot = nullptr;
    HostPlayerPosition* oldest = nullptr;
    for (HostPlayerPosition& player : occupancy.players) {
        if (player.used && tick - player.tick > kHostPlayerStaleMs) {
            player = {};
        }
        if (player.used && player.playerKey == playerKey) {
            slot = &player;
        }
    }
    for (HostPlayerPosition& player : occupancy.players) {
        if (slot != nullptr) {
            break;
        }
        if (!player.used) {
            slot = &player;
        } else if (oldest == nullptr || player.tick < oldest->tick) {
            oldest = &player;
        }
    }
    if (slot == nullptr) {
        slot = oldest;
    }
    *slot = {playerKey, position, tick, true};
    evaluate_monitors(instance, tick);
}

void note_host_occupancy_conditions(RuntimeInstance& instance,
                                    std::span<const mission_state::TypedIntent> intents) noexcept {
    namespace auth = middleware::bap::activity_message::auth_fields;
    for (const mission_state::TypedIntent& intent : intents) {
        if (intent.authSchema != format::kOccupancyAuthSchema) {
            continue;
        }
        // The body is the filter's client reference, then the caller value as a biased int32.
        middleware::encoding::bits::Reader reader(intent.authBody);
        std::uint64_t filterKey = 0;
        std::uint64_t encodedValue = 0;
        if (!reader.read(auth::kClientRefKeyWidth, filterKey)
            || !reader.skip(auth::kClientRefTypeWidth + auth::kClientRefIndexWidth)
            || !reader.read(32, encodedValue)) {
            continue;
        }
        resolve_monitors(instance);
        HostMonitor* const monitor = find_monitor(
            instance.hostOccupancy, intent.registryKey, intent.objectTag, intent.slotIndex);
        if (monitor == nullptr) {
            continue;
        }
        monitor->value =
            static_cast<std::int32_t>(static_cast<std::uint32_t>(encodedValue) - 0x80000000U);
        const bool filtered = filterKey != auth::kClientRefAbsentKey;
        if (monitor->filtered == filtered) {
            continue;
        }
        monitor->filtered = filtered;
        log_line(core::log::Level::info,
                 &instance,
                 "host_occupancy",
                 filtered ? "filtered" : "unfiltered");
        if (!filtered) {
            continue;
        }
        // The host's level no longer applies; the client's alone decides from here.
        TriggerOccupancy* const row = existing_row(instance, *monitor);
        if (row != nullptr && row->hostOccupied) {
            row->hostOccupied = false;
            if (settle_trigger_occupancy(*row, false, true)) {
                push_host_edge(instance, *monitor, *row, 0, false, GetTickCount64());
            }
            if (!row->occupied && !row->clientReported) {
                *row = {};
            }
        }
    }
}

} // namespace sunrise::server::activity::mission
