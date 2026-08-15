#pragma once

#include <cstddef>
#include <type_traits>

#include "layout.h"

namespace sunrise::middleware::datagen::family4::character::abi {

/** The selected-character SOID starts at native byte 0. */
inline constexpr std::size_t kCharacterSoidOffset = 0;
/** Race, gender, and class begin after the 8-byte selected-character SOID. */
inline constexpr std::size_t kIdentityOffset = 8;
/** The next inventory mutation serial begins at native byte 48. */
inline constexpr std::size_t kNextInventorySerialOffset = 48;
/** The first fixed inventory row begins at native byte 56. */
inline constexpr std::size_t kInventoryItemsOffset = 56;
/** Equipped instance SOIDs begin after all fixed inventory rows at native byte 11,456. */
inline constexpr std::size_t kEquippedInstancesOffset = 11456;
/** The stored equipment summary begins at native byte 11,616. */
inline constexpr std::size_t kEquipmentSummaryOffset = 11616;
/** The inventory-presence definition gate begins at native byte 12,008. */
inline constexpr std::size_t kInventoryGateDefinitionOffset = 12008;
/** The inventory gate state begins at native byte 12,024. */
inline constexpr std::size_t kInventoryGateStateOffset = 12024;
/** Per-character seen-message bits begin at native byte 12,044. */
inline constexpr std::size_t kSeenMessagesOffset = 12044;
/** The reserved roster mirror begins at native byte 12,088. */
inline constexpr std::size_t kRosterMirrorOffset = 12088;
/** The last Orbit destination hash begins at native byte 15,144. */
inline constexpr std::size_t kLastOrbitDestinationOffset = 15144;
/** The per-slot new-item bitmap begins at native byte 15,148. */
inline constexpr std::size_t kNewItemFlagsOffset = 15148;
/** Per-row item-instance progress watermarks begin at native byte 15,192. */
inline constexpr std::size_t kInstanceProgressWatermarksOffset = 15192;
/** The 3 preview-policy mirrors begin at native byte 17,384. */
inline constexpr std::size_t kPreviewMirrorsOffset = 17384;
/** Fixed character progression rows begin at native byte 17,392. */
inline constexpr std::size_t kProgressionsOffset = 17392;
/** Per-character acquired-state flags begin at native byte 37,704. */
inline constexpr std::size_t kAcquiredFlagsOffset = 37704;
/** Per-character objective values begin at native byte 41,800. */
inline constexpr std::size_t kObjectiveValuesOffset = 41800;
/** The content-policy bypass byte begins at native byte 46,336. */
inline constexpr std::size_t kContentBypassOffset = 46336;

static_assert(std::is_standard_layout_v<layout::Object>);
static_assert(offsetof(layout::Object, characterSoid) == kCharacterSoidOffset);
static_assert(offsetof(layout::Object, identity) == kIdentityOffset);
static_assert(offsetof(layout::Object, nextInventorySerial) == kNextInventorySerialOffset);
static_assert(offsetof(layout::Object, inventoryItems) == kInventoryItemsOffset);
static_assert(offsetof(layout::Object, equippedInstanceSoids) == kEquippedInstancesOffset);
static_assert(offsetof(layout::Object, equipmentSummary) == kEquipmentSummaryOffset);
static_assert(offsetof(layout::Object, inventoryGateDefinitionIndex)
              == kInventoryGateDefinitionOffset);
static_assert(offsetof(layout::Object, inventoryGateState) == kInventoryGateStateOffset);
static_assert(offsetof(layout::Object, seenMessages) == kSeenMessagesOffset);
static_assert(offsetof(layout::Object, rosterMirror) == kRosterMirrorOffset);
static_assert(offsetof(layout::Object, lastOrbitedDestination) == kLastOrbitDestinationOffset);
static_assert(offsetof(layout::Object, newItemFlags) == kNewItemFlagsOffset);
static_assert(offsetof(layout::Object, instanceProgressWatermarks)
              == kInstanceProgressWatermarksOffset);
static_assert(offsetof(layout::Object, previewMirrors) == kPreviewMirrorsOffset);
static_assert(offsetof(layout::Object, progressions) == kProgressionsOffset);
static_assert(offsetof(layout::Object, acquiredFlags) == kAcquiredFlagsOffset);
static_assert(offsetof(layout::Object, objectiveValues) == kObjectiveValuesOffset);
static_assert(offsetof(layout::Object, contentBypass) == kContentBypassOffset);

} // namespace sunrise::middleware::datagen::family4::character::abi
