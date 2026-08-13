#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sunrise::state::build_data::items::details {

/**
 * Equippable installed-build items plus their initial plugs. Collection reacquisition names a
 * native index, so the server must be able to instantiate items that were not present in the
 * initial settings file.
 */
inline constexpr std::size_t kDefinitionCapacity = 16'384;
/** Family item instances have 12 fixed ordinary socket lanes. */
inline constexpr std::size_t kInitialPlugCapacity = 12;
/** Native equipment ids run from 0 to 19. */
inline constexpr std::size_t kEquipmentSlotCount = 20;
/** All item-index bits set mean the socket lane is declared empty. */
inline constexpr std::uint16_t kUnavailableItemIndex = 0xFFFF;
/** Socket-entry-list row 0 is the native empty selection. */
inline constexpr std::uint16_t kEmptySocketEntryListIndex = 0;
/** Stored per-item stat rows. Shipped items measured declare far fewer than this. */
inline constexpr std::size_t kStatCapacity = 16;
/** All bits set means no stat row, because row 0 is a real stat. */
inline constexpr std::uint8_t kEmptyStatRow = 0xFF;
/** Sandbox perk indices one definition declares. The widest shipped item declares 4. */
inline constexpr std::size_t kSandboxPerkCapacity = 4;
/** Material override rows one definition declares over its three art stages. */
inline constexpr std::size_t kRenderOverrideCapacity = 32;
/** All bits set marks an art index the definition does not declare. */
inline constexpr std::uint16_t kUnavailableArtIndex = 0xFFFF;
/** All bits set marks a socket lane whose type the definition does not declare. */
inline constexpr std::uint16_t kUnavailableSocketType = 0xFFFF;
/** A signed material override key is empty at -1. */
inline constexpr std::int8_t kEmptyOverrideKey = -1;

/** One installed per-item stat contribution, summed into a character's stat table. */
struct Stat {
    std::uint8_t row{kEmptyStatRow};
    std::int32_t value{};
};

/** One material override row, tagged with the art stage that declared it. */
struct RenderOverride {
    std::uint8_t stage{};
    std::int8_t key{kEmptyOverrideKey};
    std::uint16_t value{};
};

/** @return 12 undeclared socket types, for a safe default detail value. */
[[nodiscard]] constexpr std::array<std::uint16_t, 12> unavailable_socket_types() noexcept {
    std::array<std::uint16_t, 12> result{};
    result.fill(kUnavailableSocketType);
    return result;
}

/** Whether the installed definition carries the optional ordinary-socket block. */
enum class OrdinarySocketState : std::uint8_t {
    /** Cache value 0 records a definition with no ordinary-socket block. */
    absent = 0,
    /** Cache value 1 records a definition with a resolved ordinary-socket block. */
    present = 1,
};

/** How the native item-definition instance flag is read. */
enum class InstancedDefinitionState : std::uint8_t {
    /** Native 0 allows authored quantities up to the definition stack limit. */
    stackable = 0,
    /** Any nonzero native flag becomes one instanced item. */
    instanced = 1,
};

/** @return 12 declared-empty plug indices, for a safe default detail value. */
[[nodiscard]] constexpr std::array<std::uint16_t, kInitialPlugCapacity>
unavailable_plug_indices() noexcept {
    std::array<std::uint16_t, kInitialPlugCapacity> result{};
    result.fill(kUnavailableItemIndex);
    return result;
}

/** Installed-build fields required to generate one configured item instance. */
struct Definition {
    std::uint16_t definitionIndex{};
    /** The definition's own hash, which the character record collects for its overflow bank. */
    std::uint32_t definitionHash{};
    std::uint8_t bucketId{};
    std::int32_t maxStackSize{};
    InstancedDefinitionState instancedDefinitionState{InstancedDefinitionState::stackable};
    std::optional<std::int8_t> equipmentSlot{};
    OrdinarySocketState ordinarySocketState{OrdinarySocketState::absent};
    std::uint8_t ordinarySocketCount{};
    std::array<std::uint16_t, kInitialPlugCapacity> initialPlugIndices{unavailable_plug_indices()};
    /** Socket type per lane, which picks the overflow-hash priority the character record uses. */
    std::array<std::uint16_t, kInitialPlugCapacity> socketTypes{unavailable_socket_types()};
    std::uint16_t socketEntryListIndex{kEmptySocketEntryListIndex};
    /** Number of filled leading entries in `stats`. */
    std::uint8_t statCount{};
    std::array<Stat, kStatCapacity> stats{};
    /** Gear art definition index the render row publishes. */
    std::uint16_t gearArtIndex{kUnavailableArtIndex};
    /** Art arrangement index the render row publishes beside it. */
    std::uint16_t artArrangementIndex{kUnavailableArtIndex};
    /** Number of filled leading entries in `sandboxPerks`. */
    std::uint8_t sandboxPerkCount{};
    std::array<std::uint16_t, kSandboxPerkCapacity> sandboxPerks{};
    /** Number of filled leading entries in `renderOverrides`. */
    std::uint8_t renderOverrideCount{};
    std::array<RenderOverride, kRenderOverrideCapacity> renderOverrides{};
};

} // namespace sunrise::state::build_data::items::details
