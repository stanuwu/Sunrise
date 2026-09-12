#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "quest_initialization.h"

namespace sunrise::state::build_data::items {

/** Signed native definition indices give 32,768 item rows. */
inline constexpr std::size_t kDefinitionCapacity = 32768;
/** All bucket bits set mark a valid item row whose inventory bucket was not found. */
inline constexpr std::uint8_t kUnresolvedBucketId = 0xFF;
/** A plug with no authored insertion/enabled price carries all set-index bits. */
inline constexpr std::uint16_t kUnavailableMaterialRequirementSetIndex = 0xFFFFU;
/** An item-index field naming no row carries all bits set. */
inline constexpr std::uint16_t kUnavailableLinkedPlugIndex = 0xFFFFU;

/** One installed-build item identity, used to look up authored definition hashes. */
struct Definition {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{kUnresolvedBucketId};
    std::uint16_t insertionMaterialRequirementSetIndex{kUnavailableMaterialRequirementSetIndex};
    std::uint16_t enabledMaterialRequirementSetIndex{kUnavailableMaterialRequirementSetIndex};
    /** Native rarity ladder: 1 common through 5 exotic; 0 outside the ladder. */
    std::uint8_t tier{};
    /** Plug category the definition declares, or 0 when it declares none. */
    std::uint32_t plugCategoryHash{};
    /**
     * Ordinal of the server roll set that grants this plug in place of a socket's action plug;
     * kNoRollSet when the plug is socketed directly, kForeignRollSet when the service granted it
     * by other means.
     */
    std::uint16_t rollSetIndex{};
    /** Item index of the plug this one stands for, or kUnavailableLinkedPlugIndex when it stands
     * alone. */
    std::uint16_t linkedPlugIndex{kUnavailableLinkedPlugIndex};
    /** Empty unless this item is the first member of a supported quest set. */
    QuestInitialization questInitialization{};
};

/** Roll-set ordinals outside the rolled ladder. */
inline constexpr std::uint16_t kNoRollSet = 0;
inline constexpr std::uint16_t kForeignRollSet = 0xFFFFU;
/** @return True when a definition is a plug the service rolled from a socket action. */
[[nodiscard]] constexpr bool rolled_result(const Definition& definition) noexcept {
    return definition.rollSetIndex != kNoRollSet && definition.rollSetIndex != kForeignRollSet;
}

/** Native item tiers, as the definition's rarity byte encodes them. */
enum class Tier : std::uint8_t {
    none = 0,
    common = 1,
    uncommon = 2,
    rare = 3,
    legendary = 4,
    exotic = 5,
};

/** Clears every generated item mapping. */
void clear() noexcept;

/**
 * Checks one complete dense item definition table.
 * @param definitions Candidate installed-build mappings.
 * @return True when every native index appears exactly once.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated item definition table in one step.
 * @param definitions Complete dense installed-build mappings.
 * @return True when all rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one authored hash with its expected inventory bucket.
 * @param definitionHash Authored item definition hash.
 * @param bucketId Expected inventory bucket id.
 * @param definition Receives the one matching mapping.
 * @return True only when exactly one mapping with a known bucket matches both keys.
 */
[[nodiscard]] bool
find(std::uint32_t definitionHash, std::uint8_t bucketId, Definition& definition) noexcept;

/**
 * Finds one authored hash without needing a known inventory bucket.
 * @param definitionHash Authored item or plug definition hash.
 * @param definition Receives the one matching installed-build mapping.
 * @return True only when exactly one native row carries the hash.
 */
[[nodiscard]] bool find_hash(std::uint32_t definitionHash, Definition& definition) noexcept;

/**
 * Finds one dense installed-build row by its native definition index.
 * @param definitionIndex Native item-definition row index.
 * @param definition Receives the matching installed-build mapping.
 * @return True when the table holds that row.
 */
[[nodiscard]] bool find_index(std::uint16_t definitionIndex, Definition& definition) noexcept;

/**
 * Copies every mapping in native definition-index order.
 * @param output Caller-owned fixed mapping storage.
 * @param count Receives the copied row count.
 * @return False only when the caller's storage cannot hold the whole table.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of installed-build item mappings. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::items
