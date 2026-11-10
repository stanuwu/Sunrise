#include "inventory_state.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <optional>
#include <string_view>

#include "../../build_data/runtime.h"

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
    {"artifact", EquipmentSlot::artifact},
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
           && item.quantity > 0 && item.mutationSerial >= 0 && valid(item.sockets)
           && (item.placement == ItemPlacement::authored
               || (item.placement == ItemPlacement::recovery && item.quantity == 1));
}

/** Checks every item present in the fixed semantic equipment array. */
bool valid(const Equipment& equipment) noexcept {
    for (const std::optional<Item>& item : equipment.slots) {
        if (item.has_value() && (!valid(*item) || item->placement != ItemPlacement::authored)) {
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
        } else if (items.values[index].instanceSoid != 0
                   || items.values[index].placement != ItemPlacement::authored) {
            return false;
        }
    }
    return true;
}

/** Checks a dense character stack list with distinct mutation identities. */
bool valid(const CharacterStacks& items) noexcept {
    if (items.count > items.values.size()) {
        return false;
    }
    for (std::size_t index = 0; index < items.values.size(); ++index) {
        const CharacterStack& item = items.values[index];
        if (index >= items.count) {
            if (item.definitionHash != 0 || item.quantity != 0 || item.mutationSerial != 0
                || item.placement != ItemPlacement::authored) {
                return false;
            }
            continue;
        }
        if (item.definitionHash == kNoDefinitionHash || item.quantity <= 0
            || item.mutationSerial < 0
            || (item.placement != ItemPlacement::authored
                && item.placement != ItemPlacement::recovery)) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (items.values[prior].mutationSerial == item.mutationSerial) {
                return false;
            }
        }
    }
    return true;
}

namespace {

/** @return The installed profile bucket an item's rows sit in. */
[[nodiscard]] bool profile_bucket(const ProfileItem& item,
                                  build_data::inventory::buckets::Descriptor& bucket) noexcept {
    build_data::items::Definition definition{};
    return build_data::find_item_definition_hash(item.definitionHash, definition)
           && build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
           && bucket.arraySelector == build_data::inventory::buckets::ArraySelector::profile;
}

} // namespace

/** Places the items that kept a slot first, so an older item never takes a saved one. */
bool resolve_profile_rows(std::span<const ProfileItem> items,
                          std::span<std::uint16_t> rows) noexcept {
    if (rows.size() < items.size()) {
        return false;
    }
    std::bitset<kProfileItemCapacity> taken{};
    for (const bool saved : {true, false}) {
        for (std::size_t index = 0; index < items.size(); ++index) {
            const ProfileItem& item = items[index];
            build_data::inventory::buckets::Descriptor bucket{};
            if ((item.slot != kUnassignedProfileSlot) != saved) {
                continue;
            }
            if (!profile_bucket(item, bucket)) {
                return false;
            }
            const std::size_t first = bucket.firstSlot;
            const std::size_t end = (std::min)(first + bucket.slotCount, taken.size());
            std::size_t row = saved ? first + item.slot : first;
            while (!saved && row < end && taken[row]) {
                ++row;
            }
            if (row >= end || taken[row]) {
                return false;
            }
            taken[row] = true;
            rows[index] = static_cast<std::uint16_t>(row);
        }
    }
    return true;
}

/** Each resolved row is turned back into a slot of its item's bucket. */
bool settle_profile_slots(std::span<ProfileItem> items) noexcept {
    std::array<std::uint16_t, kProfileItemCapacity> rows{};
    if (items.size() > rows.size() || !resolve_profile_rows(items, rows)) {
        return false;
    }
    for (std::size_t index = 0; index < items.size(); ++index) {
        build_data::inventory::buckets::Descriptor bucket{};
        if (items[index].slot != kUnassignedProfileSlot) {
            continue;
        }
        if (!profile_bucket(items[index], bucket)) {
            return false;
        }
        items[index].slot = static_cast<std::uint16_t>(rows[index] - bucket.firstSlot);
    }
    return true;
}

} // namespace sunrise::state::account::inventory
