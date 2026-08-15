#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sunrise::middleware::content::packages::tables::items {

/** Ordinary socket lanes an item definition can declare. */
inline constexpr std::size_t kSocketCapacity = 12;
/** All bits set marks a socket lane with no initial plug. */
inline constexpr std::uint16_t kUnavailablePlug = 0xFFFF;

/** Declared stat contributions one definition carries. Shipped rows stay far below this. */
inline constexpr std::size_t kStatCapacity = 64;
/** Sandbox perk indices one definition declares. The widest shipped item declares 4. */
inline constexpr std::size_t kSandboxPerkCapacity = 4;
/** Material override rows one definition declares over its 3 art stages. Widest shipped is 30. */
inline constexpr std::size_t kRenderOverrideCapacity = 32;
/** All bits set marks an art index the definition does not declare. */
inline constexpr std::uint16_t kUnavailableArtIndex = 0xFFFF;
/** All bits set marks a socket lane whose type the definition does not declare. */
inline constexpr std::uint16_t kUnavailableSocketType = 0xFFFF;
/** A signed material override key is empty at minus one. */
inline constexpr std::int8_t kEmptyOverrideKey = -1;

/** One material override row, tagged with the art stage that declared it. */
struct RenderOverride {
    std::uint8_t stage{};
    std::int8_t key{kEmptyOverrideKey};
    std::uint16_t value{};
};

/** One item row read from its own definition blob. */
struct Row {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{};
    std::int32_t maxStackSize{};
    bool instanced{};
    std::optional<std::int8_t> equipmentSlot{};
    std::int32_t rawEquipmentSlot{};
    bool hasSocketEntryList{};
    std::uint16_t socketEntryListIndex{};
    bool hasSockets{};
    std::uint8_t socketCount{};
    std::uint16_t initialPlugs[kSocketCapacity]{};
    std::uint16_t socketTypes[kSocketCapacity]{};
    std::uint8_t statCount{};
    std::uint8_t statRows[kStatCapacity]{};
    std::int32_t statValues[kStatCapacity]{};
    /** Gear art definition index, read straight from the art block. */
    std::uint16_t gearArtIndex{kUnavailableArtIndex};
    /** Art arrangement index, read from the art block's first art row. */
    std::uint16_t artArrangementIndex{kUnavailableArtIndex};
    std::uint8_t sandboxPerkCount{};
    std::uint16_t sandboxPerks[kSandboxPerkCapacity]{};
    std::uint8_t renderOverrideCount{};
    RenderOverride renderOverrides[kRenderOverrideCapacity]{};
};

/**
 * Reads the art indices, material override rows and sandbox perks one definition declares.
 * All three live behind self-relative blob offsets and are valid only against the serialized
 * definition, never against a loaded definition in process memory.
 * @param definition Whole item definition bytes.
 * @param row Receives the art, material override and sandbox perk fields.
 */
void read_appearance(std::span<const std::byte> definition, Row& row) noexcept;

/**
 * Reads the fixed fields of one item definition blob.
 * @param definition Whole item definition bytes.
 * @param row Receives every field the blob declares.
 * @return True when the blob is long enough to carry the fixed fields.
 */
[[nodiscard]] bool read_definition(std::span<const std::byte> definition, Row& row) noexcept;

} // namespace sunrise::middleware::content::packages::tables::items
