#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../character_record/character_record_encoder.h"
#include "../family4/character/layout.h"
#include "../family4/inventory/layout.h"
#include "../family4/progression/layout.h"

namespace sunrise::middleware::datagen::inspection {

/** The inspected character projects the family-four character record's own two bank sizes. */
inline constexpr std::size_t kEquipmentCapacity = family4::character::layout::kEquipmentCapacity;
/** See kEquipmentCapacity. */
inline constexpr std::size_t kProgressionCapacity =
    family4::character::layout::kProgressionCapacity;
/** Sizes the two native classes named below declare; the assertions hold each struct to one. */
inline constexpr std::size_t kRootSize = 0x800;
/** See kRootSize. */
inline constexpr std::size_t kCharacterSize = 0x1B48;

#pragma pack(push, 1)
/** Native family1 slot0, class8080780C: inspected account and selected character. */
struct Root {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::array<family4::progression::layout::Entry, kProgressionCapacity> progressions{};
};

/** Native family1 slot1, class80807994. Shared blocks retain their existing wire layouts. */
struct Character {
    character_record::layout::Identity identity{};
    std::array<family4::progression::layout::Entry, kProgressionCapacity> progressions{};
    std::int32_t inventorySerial{};
    std::uint32_t inventoryPadding{};
    std::array<family4::inventory::layout::Entry, kEquipmentCapacity> inventory{};
    std::array<std::uint64_t, kEquipmentCapacity> equippedSoids{};
    // Native blocks kept at their declared extents. Their contents have no reader here, so the
    // projection carries them clear rather than composing fields it cannot source.
    std::array<std::byte, 0xF4> changes{};
    std::array<std::byte, 4> changesPadding{};
    std::array<std::byte, 0x20> linkedRecord{};
    std::array<std::byte, 0x20> optionalState{};
    character_record::layout::Appearance appearance{};
    character_record::layout::Summary summary{};
};
#pragma pack(pop)

static_assert(sizeof(Root) == kRootSize);
static_assert(sizeof(Character) == kCharacterSize);
// Positions the native character class declares for the five blocks C++ packing could shift.
static_assert(offsetof(Character, inventorySerial) == 0x820);
static_assert(offsetof(Character, inventory) == 0x828);
static_assert(offsetof(Character, equippedSoids) == 0xAA8);
static_assert(offsetof(Character, appearance) == 0xC80);
static_assert(offsetof(Character, summary) == 0x1B28);

/** @return False for a zero account or character soid, or an `output` smaller than `kRootSize`. */
[[nodiscard]] bool encode_root(std::uint64_t accountSoid,
                               std::uint64_t characterSoid,
                               std::span<std::byte> output) noexcept;

/**
 * Extracts and validates the loadout's equipped items, one per equipment slot.
 * @return False for an invalid item count, a repeated or out-of-range equipment slot, a zero
 * instance soid, or two equipped items sharing one instance.
 */
[[nodiscard]] bool equipped_instances(const family4::loadout::ResolvedLoadout& loadout,
                                      family4::loadout::ResolvedInstances& output) noexcept;

/**
 * Encodes one family-four character record.
 * @param equipped Must be `equipped_instances`'s own result for `loadout`; any mismatch refuses.
 * @return False when `output` is smaller than `kCharacterSize`, an item fails validation, or the
 * family0 banner block fails to encode.
 */
[[nodiscard]] bool encode_character(const state::CharacterState& character,
                                    const family4::loadout::ResolvedLoadout& loadout,
                                    const family4::loadout::ResolvedInstances& equipped,
                                    std::int32_t light,
                                    std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::datagen::inspection
