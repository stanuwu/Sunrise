#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "../account/account_state.h"

namespace sunrise::state::vendor_rewards {

/** Build-86657 preview vendor 1016620613 lists these weapons for package 2746484552. */
inline constexpr std::array<std::uint32_t, 30> kVanguardWeapons{
    991314988U,  // Bad Omens.
    720351795U,  // Arsenic Bite-4b.
    4230993599U, // Steel Sybil Z-14.
    253196586U,  // Main Ingredient.
    4146702548U, // Outrageous Fortune.
    2957367743U, // Toil and Trouble.
    2009277538U, // The Last Dance.
    3745990145U, // Long Shadow.
    3356526253U, // Wishbringer.
    821154603U,  // Gnawing Hunger.
    3504336176U, // Night Watch.
    2199171672U, // Lonesome.
    188882152U,  // Last Perdition.
    3863882743U, // Uriel's Gift.
    4106983932U, // Elatha FR4.
    3569802112U, // The Old Fashioned.
    1529450902U, // Mos Epoch III.
    1807343361U, // Hawthorne's Field-Forged Shotgun.
    3622137132U, // Last Hope.
    3055192515U, // Timelines' Vertex.
    2257180473U, // Interference VI.
    2742838701U, // Dire Promise.
    2742838700U, // True Prophecy.
    1162247618U, // Jian 7 Rifle.
    1723380073U, // Enigma's Draw.
    2807687156U, // Distant Tumulus.
    1786797708U, // Escape Velocity.
    2857348871U, // Honor's Edge.
    1946491241U, // Truthteller.
    1835747805U, // Nature of the Beast.
};

/** Titan-only Vigil of Heroes hashes retain the same preview's item order. */
inline constexpr std::array<std::uint32_t, 5> kVanguardTitan{
    273457849U, 2009892127U, 3722981806U, 1392054568U, 3207116971U};
/** Hunter-only Vigil of Heroes hashes retain the same preview's item order. */
inline constexpr std::array<std::uint32_t, 5> kVanguardHunter{
    3761819011U, 178749005U, 2629204288U, 1524444346U, 1812185909U};
/** Warlock-only Vigil of Heroes hashes retain the same preview's item order. */
inline constexpr std::array<std::uint32_t, 5> kVanguardWarlock{
    1578461326U, 1108278178U, 332170995U, 3631862279U, 4086100104U};

/**
 * Class-specific armour must never enter another class's reward pool.
 * @param characterClass Selected character's class.
 * @return Matching armour hashes, or an empty span for an invalid class.
 */
inline std::span<const std::uint32_t> armour(CharacterClass characterClass) noexcept {
    switch (characterClass) {
    case CharacterClass::titan:
        return kVanguardTitan;
    case CharacterClass::hunter:
        return kVanguardHunter;
    case CharacterClass::warlock:
        return kVanguardWarlock;
    default:
        return {};
    }
}

} // namespace sunrise::state::vendor_rewards
