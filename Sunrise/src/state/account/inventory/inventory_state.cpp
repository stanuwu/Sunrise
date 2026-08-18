#include "inventory_state.h"

#include <array>
#include <optional>
#include <string_view>

namespace sunrise::state::account::inventory {
namespace {

/** One case-sensitive configuration name maps to one semantic equipment slot. */
struct SlotName {
    std::string_view name;
    EquipmentSlot slot;
};

/** Configuration names stay stable even when installed native mappings change. */
constexpr std::array<SlotName, kEquipmentSlotCount> kSlotNames{{
    {"kinetic", EquipmentSlot::kinetic},
    {"energy", EquipmentSlot::energy},
    {"heavy", EquipmentSlot::heavy},
    {"helmet", EquipmentSlot::helmet},
    {"gauntlets", EquipmentSlot::gauntlets},
    {"chest", EquipmentSlot::chest},
    {"legs", EquipmentSlot::legs},
    {"class_item", EquipmentSlot::classItem},
    {"ghost", EquipmentSlot::ghost},
    {"vehicle", EquipmentSlot::vehicle},
    {"ship", EquipmentSlot::ship},
    {"subclass", EquipmentSlot::subclass},
    {"clan_banner", EquipmentSlot::clanBanner},
    {"emblem", EquipmentSlot::emblem},
    {"emote", EquipmentSlot::emote},
    {"finisher", EquipmentSlot::finisher},
}};

} // namespace

/** Finds the semantic State slot for one exact JSON equipment name. */
std::optional<EquipmentSlot> slot_from_name(std::string_view name) noexcept {
    for (const SlotName& candidate : kSlotNames) {
        if (candidate.name == name) {
            return candidate.slot;
        }
    }
    return std::nullopt;
}

/** Resolves the native equipment slot a configured item detail occupies. */
bool resolve_native_equipment_slot(std::uint32_t definitionHash,
                                   const std::optional<std::int8_t>& detailEquipmentSlot,
                                   std::uint8_t& nativeSlot) noexcept {
    if (detailEquipmentSlot.has_value()) {
        if (*detailEquipmentSlot < 0) {
            return false;
        }
        nativeSlot = static_cast<std::uint8_t>(*detailEquipmentSlot);
        return true;
    }
    if (definitionHash != kEmoteCollectionDefinitionHash) {
        return false;
    }
    nativeSlot = kEmoteCollectionNativeEquipmentSlot;
    return true;
}

/** Checks the canonical socket policy and every authored plug hash. */
bool valid(const Sockets& sockets) noexcept {
    if (sockets.plugCount > sockets.plugs.size()) {
        return false;
    }
    if (sockets.policy == SocketPolicy::nativeDefaults) {
        if (sockets.plugCount != 0) {
            return false;
        }
    } else if (sockets.policy != SocketPolicy::authored) {
        return false;
    }

    for (std::size_t index = 0; index < sockets.plugs.size(); ++index) {
        const std::optional<std::uint32_t>& plug = sockets.plugs[index];
        if (index >= sockets.plugCount) {
            if (plug.has_value()) {
                return false;
            }
        } else if (plug.has_value() && *plug == kNoDefinitionHash) {
            return false;
        }
    }
    return true;
}

/** Checks one whole authored item without reading installed build data. */
bool valid(const Item& item) noexcept {
    return item.instanceSoid != 0 && item.definitionHash != kNoDefinitionHash && item.level >= 0
           && item.quantity > 0 && item.mutationSerial >= 0 && valid(item.sockets);
}

/** Checks every item present in the fixed semantic equipment array. */
bool valid(const Equipment& equipment) noexcept {
    for (const std::optional<Item>& item : equipment.slots) {
        if (item.has_value() && !valid(*item)) {
            return false;
        }
    }
    return true;
}

/** Checks the used prefix and empty tail of one character's unequipped item array. */
bool valid(const CharacterItems& items) noexcept {
    if (items.count > items.values.size()) {
        return false;
    }
    for (std::size_t index = 0; index < items.values.size(); ++index) {
        if (index < items.count) {
            if (!valid(items.values[index])) {
                return false;
            }
        } else if (items.values[index].instanceSoid != 0) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::state::account::inventory
