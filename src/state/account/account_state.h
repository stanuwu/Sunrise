#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inventory/inventory_state.h"
#include "settings/settings_state.h"

namespace sunrise::state {

/** One account can own at most the 3 playable character slots. */
inline constexpr std::size_t kCharacterCapacity = 3;

/** Stable character race values authored independently of package definition mappings. */
enum class CharacterRace : std::uint8_t {
    /** Wire value 0 is a Human character. */
    human = 0,
    /** Wire value 1 is an Awoken character. */
    awoken = 1,
    /** Wire value 2 is an Exo character. */
    exo = 2,
};

/** Stable character gender values authored independently of package definition mappings. */
enum class CharacterGender : std::uint8_t {
    /** Wire value 0 is a male character. */
    male = 0,
    /** Wire value 1 is a female character. */
    female = 1,
};

/** Stable character class values authored independently of package definition mappings. */
enum class CharacterClass : std::uint8_t {
    /** Wire value 0 is a Titan character. */
    titan = 0,
    /** Wire value 1 is a Hunter character. */
    hunter = 1,
    /** Wire value 2 is a Warlock character. */
    warlock = 2,
};

/** Default movement entry. Each subclass offers 3, as entries 4, 5 and 6 of its group. */
inline constexpr std::uint8_t kDefaultMovementAbilityEntry = 4;
/** No socket entry list declares more entries than this, so a larger value is not an entry. */
inline constexpr std::uint8_t kMaximumMovementAbilityEntry = 63;

/**
 * Socket entries of the other abilities a subclass lets the player choose. Each names one entry
 * of that ability's group. The subclass offers several and the character picks one. These
 * defaults are the first option of each group, where every shipped subclass starts.
 */
inline constexpr std::uint8_t kDefaultGrenadeAbilityEntry = 7;
/** Default super entry. It is the lane that carries no plug source and kind 34. */
inline constexpr std::uint8_t kDefaultSuperAbilityEntry = 10;
/** Default melee entry. */
inline constexpr std::uint8_t kDefaultMeleeAbilityEntry = 11;
/** Default class-ability entry. Which bucket it publishes into follows the character class. */
inline constexpr std::uint8_t kDefaultClassAbilityEntry = 2;

/** Authored state for one playable character slot. */
struct CharacterState {
    std::uint64_t soid{};
    /** Runtime selection, moved only by the character pick. No character is selected at boot. */
    bool selected{};
    CharacterRace race{CharacterRace::human};
    CharacterGender gender{CharacterGender::male};
    CharacterClass characterClass{CharacterClass::titan};
    std::uint8_t level{};
    bool accepted{};
    bool previewAvailable{};
    /** Authored scalar kept for the family-specific character presentation encoders. */
    float appearanceValue{};
    /** Compact default destination hash used until a later runtime selection replaces it. */
    std::uint32_t lastOrbitedDestination{};
    /** Server policy that arms content checks only with the matching family-5 flag. */
    bool contentBypass{};
    /**
     * Socket-entry-list entry naming the movement ability this character has selected.
     * One subclass group holds several movement entries, and the selected one decides which
     * ability buckets the character record publishes. A player choice, so it is authored.
     */
    std::uint8_t movementAbilityEntry{kDefaultMovementAbilityEntry};
    /** Socket entry naming the grenade this character has selected. */
    std::uint8_t grenadeAbilityEntry{kDefaultGrenadeAbilityEntry};
    /** Socket entry naming the super this character has selected. */
    std::uint8_t superAbilityEntry{kDefaultSuperAbilityEntry};
    /** Socket entry naming the melee this character has selected. */
    std::uint8_t meleeAbilityEntry{kDefaultMeleeAbilityEntry};
    /** Socket entry naming the class ability this character has selected. */
    std::uint8_t classAbilityEntry{kDefaultClassAbilityEntry};
    /** Authored loadout keyed only by stable semantic equipment slots. */
    account::inventory::Equipment equipment;
};

/** Account identity shared by backend object families. */
struct AccountState {
    std::uint64_t primarySoid{};
    /** Account-wide currencies and materials, placed by bucket rather than by authored slot. */
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity>
        profileItems{};
    std::size_t profileItemCount{};
    std::array<CharacterState, kCharacterCapacity> characters{};
    std::size_t characterCount{};
    account::settings::AccountSettings settings;
};

namespace account {

[[nodiscard]] bool valid(const AccountState& state) noexcept;

[[nodiscard]] std::uint64_t selected_character_soid(const AccountState& state) noexcept;

} // namespace account

} // namespace sunrise::state
