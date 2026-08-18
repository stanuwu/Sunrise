#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sunrise::state::account::inventory {

/** Authored equipment exposes the 16 named slots the first State supports. */
enum class EquipmentSlot : std::uint8_t {
    kinetic,
    energy,
    heavy,
    helmet,
    gauntlets,
    chest,
    legs,
    classItem,
    ghost,
    vehicle,
    ship,
    subclass,
    clanBanner,
    emblem,
    emote,
    finisher,
    count,
};

/** One fixed entry exists for every semantic equipment slot. */
inline constexpr std::size_t kEquipmentSlotCount = static_cast<std::size_t>(EquipmentSlot::count);
/** An authored item can pick at most 12 ordinary socket lanes. */
inline constexpr std::size_t kPlugCapacity = 12;
/** The engine no-definition hash cannot identify an authored item or plug. */
inline constexpr std::uint32_t kNoDefinitionHash = 0x811C9DC5U;

/** Says whether Middleware uses native socket defaults or authored lanes. */
enum class SocketPolicy : std::uint8_t {
    nativeDefaults,
    authored,
};

/** Fixed authored socket choices. An empty optional is an explicit empty lane. */
struct Sockets {
    SocketPolicy policy{SocketPolicy::nativeDefaults};
    std::array<std::optional<std::uint32_t>, kPlugCapacity> plugs{};
    std::size_t plugCount{};
};

/** Account-wide stacks can occupy every row of the native 701-row profile inventory. */
inline constexpr std::size_t kProfileItemCapacity = 701;
/** The supported mod and shader profile bucket runs reserve 50 action-source rows each. */
inline constexpr std::size_t kProfileActionSourceCapacity = 100;
/** Runtime-owned SOIDs for profile stacks use a namespace separate from created item instances. */
inline constexpr std::uint64_t kFirstProfileItemInstanceSoid = 0x5000000000000001ULL;
/**
 * Native item-state bit the Client sets to lock one item against destruction.
 * Confirmed against the installed build by three observed lock and unlock transitions.
 */
inline constexpr std::uint32_t kLockedItemFlag = 0x1;
/**
 * The 16 supported character equipment buckets reserve 151 native rows in this build. One row

 * * per semantic slot can be equipped, leaving at most 135 simultaneously unequipped instances.
 */
inline constexpr std::size_t kCharacterItemCapacity = 135;

/**
 * Definition hash of the real, non-equippable "Emotes" collection item. The Client opens its own
 * wheel-configuration screen for this exact item once it is equipped with valid socket data.
 */
inline constexpr std::uint32_t kEmoteCollectionDefinitionHash = 3183180185U;
/** Ordinary socket lane count the "Emotes" collection item's real content declares. */
inline constexpr std::size_t kEmoteCollectionSocketLaneCount = 4;
/**
 * Native equipment slot the "Emotes" collection item is equipped under, in place of the individual
 * emote it replaces. Its own real content carries no native equipment-slot mapping at all, unlike
 * every other character-scoped item, so callers that need one for this item specifically fall back
 * to this constant through resolve_native_equipment_slot() below.
 */
inline constexpr std::uint8_t kEmoteCollectionNativeEquipmentSlot =
    static_cast<std::uint8_t>(EquipmentSlot::emote);

/**
 * Resolves the native equipment slot a configured item detail occupies.
 * Every character-scoped item declares its own native slot except the "Emotes" collection item
 * (kEmoteCollectionDefinitionHash), the one item whose real content has none. Any other item
 * missing a native slot is rejected instead of silently aliasing this fallback.
 * @param definitionHash Authored item definition hash being resolved.
 * @param detailEquipmentSlot The installed item detail's own native slot, if it declares one.
 * @param nativeSlot Receives the resolved native slot on success.
 * @return True when the item declares its own non-negative slot, or is the Emotes collection item.
 */
[[nodiscard]] bool
resolve_native_equipment_slot(std::uint32_t definitionHash,
                              const std::optional<std::int8_t>& detailEquipmentSlot,
                              std::uint8_t& nativeSlot) noexcept;

/** One authored account-wide item, placed by the inventory bucket its definition names. */
struct ProfileItem {
    /** Stable runtime identity required to materialize this row as an inventory action source. */
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    /** Rising generation copied into the native row and matched by acquisition feedback. */
    std::int32_t mutationSerial{};
};

/** One authored equipment item without native table or wire-layout fields. */
struct Item {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t level{};
    std::int32_t quantity{};
    /** Rising per-character generation assigned whenever this item changes inventory rows. */
    std::int32_t mutationSerial{};
    /** Native accumulated item-state bits such as the finisher favorite marker. */
    std::uint32_t flags{};
    Sockets sockets;
};

/** Ordered unequipped items placed into their native character-inventory bucket ranges. */
struct CharacterItems {
    std::array<Item, kCharacterItemCapacity> values{};
    std::size_t count{};
};

/** One optional authored item for every semantic equipment slot. */
struct Equipment {
    std::array<std::optional<Item>, kEquipmentSlotCount> slots{};
};

/**
 * Finds the semantic State slot for one exact JSON equipment name.
 * @param name Borrowed case-sensitive equipment name.
 * @return Matching slot, or no value for an unknown name.
 */
[[nodiscard]] std::optional<EquipmentSlot> slot_from_name(std::string_view name) noexcept;

/**
 * Checks the canonical socket policy and every authored plug hash.
 * @return True when the policy, count and fixed tail agree.
 */
[[nodiscard]] bool valid(const Sockets& sockets) noexcept;

/**
 * Checks one whole authored item without reading installed build data.
 * @return True when the id, scalar and socket fields are valid.
 */
[[nodiscard]] bool valid(const Item& item) noexcept;

/**
 * Checks every item present in the fixed semantic equipment array.
 * @return True when every used slot holds a whole item.
 */
[[nodiscard]] bool valid(const Equipment& equipment) noexcept;

/** Checks the used prefix and empty tail of one character's unequipped item array. */
[[nodiscard]] bool valid(const CharacterItems& items) noexcept;

} // namespace sunrise::state::account::inventory
