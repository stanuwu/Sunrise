#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../state/account/account_state.h"

namespace sunrise::middleware::datagen::roster {

/** Replicated account rosters reserve 10 fixed character rows. */
inline constexpr std::size_t kCharacterCapacity = 10;
/** Each character row reserves 20 fixed equipment references. */
inline constexpr std::size_t kEquipmentCapacity = 20;

/** One 8-byte equipment pair in a fixed character row. */
struct EquipmentReference {
    std::uint16_t definitionIndex{};
    std::uint16_t reserved{};
    std::int32_t value{};
};

/** One character key followed by all 20 fixed equipment pairs. */
struct CharacterEntry {
    std::uint64_t characterSoid{};
    std::array<EquipmentReference, kEquipmentCapacity> equipment{};
};

/** Shared count and fixed rows embedded by account-level roster objects. */
struct Block {
    std::uint32_t characterCount{};
    std::uint32_t reserved{};
    std::array<CharacterEntry, kCharacterCapacity> characters{};
};

/**
 * Builds one roster block from authored State identities and empty equipment.
 * @param account Validated account and character identities.
 * @param block Native roster block to initialize.
 * @return True when the authored character list fits the replicated rows.
 */
[[nodiscard]] bool initialize(const state::AccountState& account, Block& block) noexcept;

static_assert(sizeof(EquipmentReference) == 2 * sizeof(std::uint16_t) + sizeof(std::int32_t));
static_assert(sizeof(CharacterEntry)
              == sizeof(std::uint64_t) + kEquipmentCapacity * sizeof(EquipmentReference));
static_assert(sizeof(Block)
              == 2 * sizeof(std::uint32_t) + kCharacterCapacity * sizeof(CharacterEntry));

} // namespace sunrise::middleware::datagen::roster
