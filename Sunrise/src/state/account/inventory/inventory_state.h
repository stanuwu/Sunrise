#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "../../build_data/inventory/buckets/definition.h"
#include "item_state.h"

namespace sunrise::state::account::inventory {

/** Authored equipment exposes every named slot currently represented by State. */
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
    artifact,
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
/** Unequipped instances can occupy every character row not reserved for equipped items. */
inline constexpr std::size_t kCharacterItemCapacity =
    build_data::inventory::buckets::kCharacterSlotCapacity - kEquipmentSlotCount;
/** Stacks share those unequipped rows with instances; encoding enforces each bucket span. */
inline constexpr std::size_t kCharacterStackCapacity = kCharacterItemCapacity;

/**
 * Definition hash of the real, non-equippable "Emotes" collection item. The Client opens its own
 * wheel-configuration screen for this exact item once it is equipped with valid socket data.
 */
inline constexpr std::uint32_t kEmoteCollectionDefinitionHash = 3183180185U;
/** Ordinary socket lane count the "Emotes" collection item's real content declares. */
inline constexpr std::size_t kEmoteCollectionSocketLaneCount = 4;
/**
 * Native equipment slot the "Emotes" collection item is equipped under.
 * It is the one character-scoped item whose content declares no slot of its own.
 */
inline constexpr std::uint8_t kEmoteCollectionNativeEquipmentSlot =
    static_cast<std::uint8_t>(EquipmentSlot::emote);

/**
 * Resolves the native equipment slot a configured item detail occupies.
 * Only kEmoteCollectionDefinitionHash may fall back to the constant above; any other item with no
 * declared slot is rejected rather than aliased onto it.
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
    /** The client has dismissed this item's new-item marker. */
    bool seen{};
};

/** Saved ownership location, independent of the item's normal definition bucket. */
enum class ItemPlacement : std::uint8_t { inventory, lostItems };

/** One authored equipment item without native table or wire-layout fields. */
struct Item {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t level{};
    std::int32_t quantity{};
    /**
     * Rising per-character generation assigned whenever this item changes inventory rows. The
     * Client also orders a bucket's grid by it, so an equip swap hands the displaced item the
     * clicked item's prior serial to keep it in the clicked cell.
     */
    std::int32_t mutationSerial{};
    /** Native accumulated item-state bits such as the finisher favorite marker. */
    std::uint32_t flags{};
    Sockets sockets;
    /**
     * Selected ability-node socket entries. Only meaningful on a subclass. Kept on the item, not
     * the character, so each owned subclass remembers its own picks. Defaults match
     * state::kDefault*AbilityEntry, literal here to avoid a circular include.
     */
    std::uint8_t movementAbilityEntry{4};
    std::uint8_t grenadeAbilityEntry{7};
    std::uint8_t superAbilityEntry{10};
    std::uint8_t meleeAbilityEntry{11};
    std::uint8_t classAbilityEntry{2};
    /** The client has dismissed this item's new-item marker. */
    bool seen{};
    ItemPlacement placement{ItemPlacement::inventory};
};

/** Ordered unequipped items placed into their native character-inventory bucket ranges. */
struct CharacterItems {
    std::array<Item, kCharacterItemCapacity> values{};
    std::size_t count{};
};

struct CharacterStack {
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
};

struct CharacterStacks {
    std::array<CharacterStack, kCharacterStackCapacity> values{};
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

[[nodiscard]] bool valid(const CharacterStacks& items) noexcept;

} // namespace sunrise::state::account::inventory
