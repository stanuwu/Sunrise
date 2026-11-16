#include "mission_script_player_trigger.h"

#include <cstddef>
#include <cstdint>

#include "mission_script_region.h"

namespace sunrise::server::activity::mission::player_trigger {
namespace {

namespace catalog = state::build_data::scriptables;

// A player trigger arrives on the type-31 volume slot and resolves to a type-60 target slot.
constexpr std::uint16_t kSourceSlotType = 31;
constexpr std::uint8_t kTargetSlotType = 60;

[[nodiscard]] bool same_source(const Source& left, const Source& right) noexcept {
    return left.registryKey == right.registryKey && left.objectTag == right.objectTag
           && left.volumeRegistryKey == right.volumeRegistryKey
           && left.sourceObjectRow == right.sourceObjectRow
           && left.sourceSlotRow == right.sourceSlotRow
           && left.volumeSlotIndex == right.volumeSlotIndex && left.slotIndex == right.slotIndex
           && left.volumeSlotType == right.volumeSlotType && left.slotType == right.slotType;
}

} // namespace

/** Resolves the type-31 volume the payload names and its generated type-60 target. */
ResolveStatus
resolve(const catalog::Snapshot& snapshot,
        const middleware::bap::activity_message::player_trigger_incident::Payload& payload,
        Source& output,
        std::int32_t region) noexcept {
    output = {};
    bool found = false;
    for (std::size_t ownerRow = 0; ownerRow < snapshot.triggerVolumeOwners.size(); ++ownerRow) {
        const catalog::TriggerVolumeOwner& owner = snapshot.triggerVolumeOwners[ownerRow];
        if (owner.tableRow >= snapshot.triggerVolumeTables.size()
            || owner.objectRow >= snapshot.objects.size()) {
            return ResolveStatus::invalidCatalog;
        }
        const catalog::TriggerVolumeTable& table = snapshot.triggerVolumeTables[owner.tableRow];
        if (table.slotType != kTargetSlotType) {
            continue;
        }
        const std::size_t first = owner.firstIncomingReference;
        const std::size_t count = owner.incomingReferenceCount;
        if (first > snapshot.triggerVolumeIncomingReferences.size()
            || count > snapshot.triggerVolumeIncomingReferences.size() - first) {
            return ResolveStatus::invalidCatalog;
        }
        for (std::size_t offset = 0; offset < count; ++offset) {
            const catalog::TriggerVolumeIncomingReference& incoming =
                snapshot.triggerVolumeIncomingReferences[first + offset];
            if (incoming.ownerRow != ownerRow || incoming.sourceObjectRow >= snapshot.objects.size()
                || incoming.sourceSlotRow >= snapshot.slots.size()) {
                return ResolveStatus::invalidCatalog;
            }
            const catalog::Object& object = snapshot.objects[incoming.sourceObjectRow];
            const catalog::Slot& slot = snapshot.slots[incoming.sourceSlotRow];
            if (slot.objectRow != incoming.sourceObjectRow || slot.slotType != kSourceSlotType
                || object.registryKey != payload.registryKey
                || slot.slotType != static_cast<std::uint8_t>(payload.slotType)
                || slot.slotIndex != static_cast<std::uint16_t>(payload.slotIndex)
                || (region != kAnyRegion && !occurrence_in_region(snapshot, object, region))) {
                continue;
            }
            const Source candidate{object.registryKey,
                                   object.objectTag,
                                   table.registryKey,
                                   incoming.sourceObjectRow,
                                   incoming.sourceSlotRow,
                                   table.slotIndex,
                                   slot.slotIndex,
                                   table.slotType,
                                   static_cast<std::uint8_t>(slot.slotType)};
            if (found && !same_source(output, candidate)) {
                output = {};
                return ResolveStatus::ambiguous;
            }
            output = candidate;
            found = true;
        }
    }
    return found ? ResolveStatus::ready : ResolveStatus::absent;
}

} // namespace sunrise::server::activity::mission::player_trigger
