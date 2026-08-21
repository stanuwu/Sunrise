#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "inventory/inventory_state.h"
#include "settings/settings_state.h"

namespace sunrise::state {

/** One account can own at most the 3 playable character slots. */
inline constexpr std::size_t kCharacterCapacity = 3;
/** A server-authored dismantle policy: a few rows per rarity and gear class. */
inline constexpr std::size_t kDismantleRewardPolicyCapacity = 32;

/** Gear classes a dismantle payout row can be limited to. */
enum class DismantleGearClass : std::uint8_t {
    weapon = 1U << 0U,
    armor = 1U << 1U,
};

/** Whether a payout row wants the dismantled item masterworked. */
enum class DismantleMasterworkFilter : std::uint8_t {
    any = 0,
    masterworked = 1,
    notMasterworked = 2,
};

/**
 * One profile material credited when ordinary character gear is dismantled. Every filter left at
 * its "any" value matches every item; the payout is the sum of the matching rows.
 */
struct DismantleRewardPolicy {
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    /** Bit (1 << tier) per native tier 1-5 the row pays for; 0 pays for every tier. */
    std::uint8_t tierMask{};
    /** DismantleGearClass bits the row pays for; 0 pays for both. */
    std::uint8_t classMask{};
    DismantleMasterworkFilter masterwork{DismantleMasterworkFilter::any};
};

/** @return True when both rows are the same row: same material under the same filters. */
[[nodiscard]] constexpr bool
same_dismantle_policy_key(const DismantleRewardPolicy& left,
                          const DismantleRewardPolicy& right) noexcept {
    return left.definitionHash == right.definitionHash && left.tierMask == right.tierMask
           && left.classMask == right.classMask && left.masterwork == right.masterwork;
}

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

/** Native character-select presentation block retained from character creation. */
inline constexpr std::size_t kCharacterPresentationHeaderSize = 36;
/** Native-width creator block provisionally associated with character creation publication. */
inline constexpr std::size_t kCharacterCreationHeaderSize = 36;
/** Final native-width creator block retained losslessly for its eventual authored consumer. */
inline constexpr std::size_t kCharacterCreationTailSize = 24;

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

/**
 * Semantic ability-bucket destinations shared by the wire encoder and the selection logic that
 * routes a clicked socket entry to a character field. A subclass entry's authored selector chain,
 * not its table position, decides which it reaches; a bundled pick can mix members across them.
 */
inline constexpr std::uint8_t kGrenadeAbilityBucket = 0;
inline constexpr std::uint8_t kSuperAbilityBucket = 1;
inline constexpr std::uint8_t kMeleeAbilityBucket = 2;
inline constexpr std::uint8_t kMovementAbilityBucket = 3;
inline constexpr std::uint8_t kSprintAbilityBucket = 4;

/**
 * Widest node bundle one summary pick can publish together.
 * Some groups (an Attunement pick, for example) hold several consecutive entries that all
 * activate, and contribute their hashes, as one unit rather than a single alternative per group.
 */
inline constexpr std::size_t kMaxAttunementBundleSize = 4;

/** @param characterClass Authored class. @return The bucket its class ability publishes into. */
[[nodiscard]] inline constexpr std::uint8_t
class_ability_bucket(CharacterClass characterClass) noexcept {
    switch (characterClass) {
    case CharacterClass::hunter:
        return 9;
    case CharacterClass::warlock:
        return 11;
    case CharacterClass::titan:
    default:
        return 6;
    }
}

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
     * Runtime-only socket entries the player has selected at least once. Selected entries still
     * publish active; this mask keeps a later inactive entry acquired instead of new. Unverified:
     * defaulted all-set, assuming the Client only allows clicking an already-acquired node.
     */
    std::uint64_t acquiredSubclassAbilityMask{~std::uint64_t{0}};
    /** Authored loadout keyed only by stable semantic equipment slots. */
    account::inventory::Equipment equipment;
    /** Unequipped items routed into their installed character-inventory bucket ranges. */
    account::inventory::CharacterItems inventory;
    /** Next row generation; equip transactions consume two values for the two moved items. */
    std::uint32_t nextInventorySerial{};

    /**
     * Native presentation/customization data captured from the in-game character creator.
     *
     * A cleared block means the character predates native creator capture. The fields stay opaque
     * here so the authored native consumers, rather than State, define their internal semantics.
     *
     * These fields are appended to preserve the ordering of all pre-existing aggregate members.
     */
    std::array<std::byte, kCharacterPresentationHeaderSize> presentationHeader{};
    std::array<std::byte, kCharacterCreationHeaderSize> creationHeader{};
    std::array<std::byte, kCharacterCreationTailSize> creationTail{};
    /** Opaque final five bits of the native opcode-501 creator payload. */
    std::uint8_t creatorTrailer{};
};

/** Account identity shared by backend object families. */
struct AccountState {
    std::uint64_t primarySoid{};
    /** Economy policy comes from configuration, never from item-specific runtime constants. */
    std::array<DismantleRewardPolicy, kDismantleRewardPolicyCapacity> dismantleRewards{};
    std::size_t dismantleRewardCount{};
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

/** Checks settings-authored State before runtime-only profile stack SOIDs are assigned. */
[[nodiscard]] bool valid_authored(const AccountState& state) noexcept;

[[nodiscard]] std::uint64_t selected_character_soid(const AccountState& state) noexcept;

/**
 * Reports the character the family-zero banner pair names.
 * The pair is published before any pick. A refusal spends the family's only acceptance window,
 * so it falls back to the first character.
 * @param state Account snapshot read under the lock.
 * @return The character's SOID, or zero when the account owns none.
 */
[[nodiscard]] std::uint64_t banner_character_soid(const AccountState& state) noexcept;

} // namespace account

} // namespace sunrise::state
