#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::state::content {

/** Extracted definition names longer than 127 bytes are rejected instead of truncated. */
inline constexpr std::size_t kDefinitionNameCapacity = 128;
/** A 4096-slot table keeps the supported catalog below a two-thirds load factor. */
inline constexpr std::size_t kDefinitionCatalogCapacity = 4096;

/** One name-to-definition mapping extracted from installed content. */
struct Definition {
    std::array<char, kDefinitionNameCapacity> name{};
    std::size_t nameLength{};
    std::uint32_t tag{};
    std::uint32_t classId{};
};

/** Clears all extracted mappings. */
void clear() noexcept;

/**
 * Freezes the populated catalog against later extraction inserts.
 * @return True when at least one mapping exists and the catalog is sealed.
 */
[[nodiscard]] bool seal() noexcept;

/**
 * Inserts one exact mapping, merging identical package entries.
 * @param name Extracted UTF-8 definition name.
 * @param tag Extracted definition tag.
 * @param classId Extracted definition class.
 * @return False when the name is invalid, the catalog is sealed, or storage is full.
 */
[[nodiscard]] bool insert(std::string_view name, std::uint32_t tag, std::uint32_t classId) noexcept;

/**
 * Finds every mapping with one extracted name, because names need not be unique.
 * @param name Exact UTF-8 definition name.
 * @param output Caller storage for matching mappings.
 * @param count Receives the number of mappings copied.
 * @return False only when more mappings exist than the caller can receive.
 */
[[nodiscard]] bool
lookup(std::string_view name, std::span<Definition> output, std::size_t& count) noexcept;

/**
 * Finds every mapping whose extracted name has one 32-bit FNV-1 content id.
 * @param nameHash Compact authored name id.
 * @param output Caller storage for matching mappings.
 * @param count Receives the number of mappings copied.
 * @return False only when more mappings exist than the caller can receive.
 */
[[nodiscard]] bool
lookup_hash(std::uint32_t nameHash, std::span<Definition> output, std::size_t& count) noexcept;

/**
 * Copies every used mapping into caller storage, for writing the cache.
 * @param output Caller-owned mapping storage.
 * @param count Receives the number of copied distinct mappings.
 * @return False only when caller storage cannot hold the whole catalog.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/**
 * Replaces the catalog with checked cached mappings in one step.
 * @param definitions Complete distinct mapping set loaded from one cache file.
 * @return True when every mapping is valid, distinct, and fits fixed storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/** @return Number of distinct extracted name, tag, and class tuples. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::content
