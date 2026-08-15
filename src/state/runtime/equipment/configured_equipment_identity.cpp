#include "configured_equipment_identity.h"

#include <limits>
#include <optional>

namespace sunrise::state::runtime::equipment {
namespace {

/** FNV-1a's 64-bit offset basis gives the equipment fingerprint a stable nonzero start. */
constexpr std::uint64_t kEquipmentHashOffsetBasis = 14695981039346656037ULL;
/** FNV-1a's 64-bit prime mixes each ordered equipment byte without keeping source data. */
constexpr std::uint64_t kEquipmentHashPrime = 1099511628211ULL;
/** Marker 0 marks an empty semantic equipment slot. */
constexpr std::uint8_t kAbsentItemMarker = 0;
/** Marker 1 marks a present item, even when its authored definition hash is 0. */
constexpr std::uint8_t kPresentItemMarker = 1;

/**
 * Mixes one ordered byte into the settings-sensitive equipment fingerprint.
 * @param hash Mutable 64-bit FNV-1a accumulator.
 * @param value Next canonical byte.
 */
void mix_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kEquipmentHashPrime;
}

/**
 * Mixes one 16-bit policy value in explicit least-significant-byte order.
 * @param hash Mutable 64-bit FNV-1a accumulator.
 * @param value Configured score selector.
 */
void mix_value(std::uint64_t& hash, std::uint16_t value) noexcept {
    for (std::size_t byteIndex = 0; byteIndex < sizeof value; ++byteIndex) {
        const std::size_t shift = byteIndex * (std::numeric_limits<std::uint8_t>::digits);
        mix_byte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

/**
 * Mixes one 32-bit authored or policy value in explicit least-significant-byte order.
 * @param hash Mutable 64-bit FNV-1a accumulator.
 * @param value Authored base hash, authored level, or score-policy revision.
 */
void mix_value(std::uint64_t& hash, std::uint32_t value) noexcept {
    for (std::size_t byteIndex = 0; byteIndex < sizeof value; ++byteIndex) {
        const std::size_t shift = byteIndex * (std::numeric_limits<std::uint8_t>::digits);
        mix_byte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

/**
 * Mixes one item's socket policy and every authored plug lane.
 * The extraction pass reads a detail row for each authored plug, so a changed plug must rebuild.
 * @param hash Mutable 64-bit FNV-1a accumulator.
 * @param sockets Authored socket policy and lanes.
 */
void mix_sockets(std::uint64_t& hash, const account::inventory::Sockets& sockets) noexcept {
    mix_byte(hash, static_cast<std::uint8_t>(sockets.policy));
    mix_byte(hash, static_cast<std::uint8_t>(sockets.plugCount));
    for (std::size_t lane = 0; lane < sockets.plugCount && lane < sockets.plugs.size(); ++lane) {
        if (!sockets.plugs[lane].has_value()) {
            mix_byte(hash, kAbsentItemMarker);
            continue;
        }
        mix_byte(hash, kPresentItemMarker);
        mix_value(hash, *sockets.plugs[lane]);
    }
}

/**
 * Mixes one character's 5 selected subclass entries.
 * The ability bucket rows are keyed by these, so a changed pick must rebuild.
 * @param hash Mutable 64-bit FNV-1a accumulator.
 * @param character Authored character.
 */
void mix_ability_selection(std::uint64_t& hash, const CharacterState& character) noexcept {
    mix_byte(hash, character.movementAbilityEntry);
    mix_byte(hash, character.grenadeAbilityEntry);
    mix_byte(hash, character.superAbilityEntry);
    mix_byte(hash, character.meleeAbilityEntry);
    mix_byte(hash, character.classAbilityEntry);
}

} // namespace

/** Builds a nonsecret cache identity from ordered authored equipment. */
std::uint64_t configured_hash(const AccountState& accountState) noexcept {
    std::uint64_t hash = kEquipmentHashOffsetBasis;
    mix_byte(hash, static_cast<std::uint8_t>(accountState.characterCount));
    for (std::size_t characterIndex = 0; characterIndex < accountState.characterCount;
         ++characterIndex) {
        const CharacterState& character = accountState.characters[characterIndex];
        mix_ability_selection(hash, character);
        for (const std::optional<account::inventory::Item>& item : character.equipment.slots) {
            if (!item.has_value()) {
                mix_byte(hash, kAbsentItemMarker);
                continue;
            }
            mix_byte(hash, kPresentItemMarker);
            // SOIDs, quantity, gates and secrets stay outside build identity.
            mix_value(hash, item->definitionHash);
            mix_value(hash, static_cast<std::uint32_t>(item->level));
            mix_sockets(hash, item->sockets);
        }
    }
    return hash;
}

} // namespace sunrise::state::runtime::equipment
