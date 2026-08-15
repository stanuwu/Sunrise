#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::items {

/** Signed native definition indices give 32,768 item rows. */
inline constexpr std::size_t kDefinitionCapacity = 32768;
/** All bucket bits set mark a valid item row whose inventory bucket was not found. */
inline constexpr std::uint8_t kUnresolvedBucketId = 0xFF;

/** One installed-build item identity, used to look up authored definition hashes. */
struct Definition {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{kUnresolvedBucketId};
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
