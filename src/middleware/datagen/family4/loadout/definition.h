#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../../state/account/inventory/inventory_state.h"
#include "../instance/instance_encoder.h"

namespace sunrise::middleware::datagen::family4::loadout {

/** One selected item can occupy each of the 16 authored semantic equipment slots. */
inline constexpr std::size_t kItemCapacity = state::account::inventory::kEquipmentSlotCount;

/** One installed-build-resolved item ready for character and instance encoding. */
struct ResolvedItem {
    std::uint16_t inventoryRow{};
    std::uint8_t equipmentSlot{};
    std::int32_t quantity{};
    instance::ResolvedInstance instance{};
};

/** One item instance together with the native equipment slot that owns it. */
struct SlottedInstance {
    std::uint8_t equipmentSlot{};
    instance::ResolvedInstance instance{};
};

/** Every item instance one character owns, whether or not that character is selected. */
struct ResolvedInstances {
    std::array<SlottedInstance, kItemCapacity> items{};
    std::size_t itemCount{};
};

/** Complete selected-character loadout committed only after every mapping resolves. */
struct ResolvedLoadout {
    std::array<ResolvedItem, kItemCapacity> items{};
    std::size_t itemCount{};
    std::uint32_t nextInventorySerial{};
};

} // namespace sunrise::middleware::datagen::family4::loadout
