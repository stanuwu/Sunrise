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

/** Preview vendor 1894103790 lists Titan-only Wing Discipline armour in this order. */
inline constexpr std::array<std::uint32_t, 5> kCrucibleTitan{
    657606375U, 2899275886U, 2389585538U, 687386728U, 1613581523U};
/** The same preview lists Hunter-only Wing Contender armour in this order. */
inline constexpr std::array<std::uint32_t, 5> kCrucibleHunter{
    3153956825U, 3091776080U, 1914589560U, 3408834730U, 693067797U};
/** The same preview lists Warlock-only Wing Theorem armour in this order. */
inline constexpr std::array<std::uint32_t, 5> kCrucibleWarlock{
    3684978064U, 3441081953U, 2286507447U, 641063251U, 119859462U};

/** Checked package candidates; class ordinals are Titan, Hunter and Warlock. */
struct Pool {
    std::span<const std::uint32_t> weapons;
    std::array<std::span<const std::uint32_t>, 3> classArmour;
};
/** Package 2746484552 uses preview 1016620613. */
inline constexpr Pool kVanguardPool{kVanguardWeapons,
                                    {kVanguardTitan, kVanguardHunter, kVanguardWarlock}};
/** Package 3289621657 uses preview 1894103790, with the same weapons but its own armour. */
inline constexpr Pool kCruciblePool{kVanguardWeapons,
                                    {kCrucibleTitan, kCrucibleHunter, kCrucibleWarlock}};
/** Supported previews contain at most thirty weapons and five eligible armour pieces. */
inline constexpr std::size_t kCandidateCapacity = 35;

/**
 * Class-specific armour must never enter another class's reward pool.
 * @param pool Checked package preview candidates.
 * @param characterClass Selected character's class.
 * @return Matching armour hashes, or an empty span for an invalid class.
 */
inline std::span<const std::uint32_t> armour(const Pool& pool,
                                             CharacterClass characterClass) noexcept {
    const auto index = static_cast<std::size_t>(characterClass);
    return index < pool.classArmour.size() ? pool.classArmour[index]
                                           : std::span<const std::uint32_t>{};
}

} // namespace sunrise::state::vendor_rewards
