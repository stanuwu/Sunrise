#include "spawn_set_catalog.h"

#include <algorithm>
#include <limits>
#include <string_view>

#include "../table.h"

namespace sunrise::state::build_data::spawn_sets {
namespace {

// One lock covers both tables: a stem names a range inside the hash bank, so the pair must move
// together or a stem could point past the end of the bank a reader sees.
Lock g_lock;
Table<Stem, kStemCapacity> g_stems;
Table<NameHash, kNameHashCapacity> g_nameHashes;

/** @param stem Stem row. @return Its bounded logical name. */
[[nodiscard]] std::string_view name_of(const Stem& stem) noexcept {
    return {stem.name.data(), stem.nameLength};
}

/** @param value Character. @return True for a lowercase package-stem byte. */
[[nodiscard]] bool stem_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_';
}

/**
 * Checks one stem and its contiguous name-hash range.
 * @param stem Candidate stem.
 * @param stemIndex Candidate stem index.
 * @param expectedOffset Required first hash row.
 * @param nameHashes Complete flat hash bank.
 * @return True when the row and every hash it owns are canonical.
 */
[[nodiscard]] bool canonical(const Stem& stem,
                             std::size_t stemIndex,
                             std::size_t expectedOffset,
                             std::span<const NameHash> nameHashes) noexcept {
    if (stem.nameLength == 0 || stem.nameLength > kStemNameCapacity || stem.setCount == 0
        || stem.nameHashOffset != expectedOffset
        || stem.nameHashCount > nameHashes.size() - expectedOffset) {
        return false;
    }
    for (std::size_t index = 0; index < kStemNameCapacity; ++index) {
        const bool declared = index < stem.nameLength;
        if ((declared && !stem_character(stem.name[index]))
            || (!declared && stem.name[index] != '\0')) {
            return false;
        }
    }
    std::uint64_t points = 0;
    std::uint32_t previousHash = 0;
    for (std::size_t index = 0; index < stem.nameHashCount; ++index) {
        const NameHash& row = nameHashes[expectedOffset + index];
        if (row.stemIndex != stemIndex || row.pointCount == 0
            || (index != 0 && row.value <= previousHash)) {
            return false;
        }
        previousHash = row.value;
        points += row.pointCount;
    }
    return points == stem.pointCount && points <= (std::numeric_limits<std::uint32_t>::max)();
}

} // namespace

/** Clears every extracted stem and name-hash row under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_stems.clear();
    g_nameHashes.clear();
}

/** Checks one complete spawn-set catalog in canonical stem and hash order. */
bool valid(std::span<const Stem> stems, std::span<const NameHash> nameHashes) noexcept {
    if (stems.empty() || stems.size() > kStemCapacity || nameHashes.size() > kNameHashCapacity) {
        return false;
    }
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < stems.size(); ++index) {
        if (!canonical(stems[index], index, expectedOffset, nameHashes)
            || (index != 0 && name_of(stems[index - 1]) >= name_of(stems[index]))) {
            return false;
        }
        expectedOffset += stems[index].nameHashCount;
    }
    return expectedOffset == nameHashes.size();
}

/** Replaces the complete spawn-set catalog in one step. */
bool replace(std::span<const Stem> stems, std::span<const NameHash> nameHashes) noexcept {
    if (!valid(stems, nameHashes)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    // Both run, with no short-circuit, so the pair cannot be left half replaced. valid() already
    // checked each against its size, which is the only reason either can refuse.
    const bool storedStems = g_stems.replace(stems);
    const bool storedHashes = g_nameHashes.replace(nameHashes);
    return storedStems && storedHashes;
}

/** Finds one stem by its normalized name. */
bool find(std::string_view name, Stem& stem) noexcept {
    stem = {};
    const Lock::Shared guard(g_lock);
    const std::span<const Stem> rows = g_stems.rows();
    const auto found =
        std::lower_bound(rows.begin(), rows.end(), name, [](const Stem& row, auto key) {
            return name_of(row) < key;
        });
    const bool present = found != rows.end() && name_of(*found) == name;
    if (present) {
        stem = *found;
    }
    return present;
}

/** Finds one spawn-name hash inside one stem. */
bool find_hash(std::string_view stem, std::uint32_t value, NameHash& nameHash) noexcept {
    nameHash = {};
    const Lock::Shared guard(g_lock);
    const std::span<const Stem> stemRows = g_stems.rows();
    const auto foundStem =
        std::lower_bound(stemRows.begin(), stemRows.end(), stem, [](const Stem& row, auto key) {
            return name_of(row) < key;
        });
    if (foundStem == stemRows.end() || name_of(*foundStem) != stem) {
        return false;
    }
    const std::span<const NameHash> bank = g_nameHashes.rows();
    const auto first = bank.begin() + static_cast<std::ptrdiff_t>(foundStem->nameHashOffset);
    const auto last = first + static_cast<std::ptrdiff_t>(foundStem->nameHashCount);
    const auto found = std::lower_bound(
        first, last, value, [](const NameHash& row, auto key) { return row.value < key; });
    const bool present = found != last && found->value == value;
    if (present) {
        nameHash = *found;
    }
    return present;
}

/** Copies the name-hash rows one stem owns. */
bool stem_hashes(const Stem& stem, std::span<NameHash> output, std::size_t& count) noexcept {
    count = 0;
    const Lock::Shared guard(g_lock);
    const std::span<const NameHash> bank = g_nameHashes.rows();
    const std::size_t offset = stem.nameHashOffset;
    const std::size_t rows = stem.nameHashCount;
    // A stem copied before a replace can name a range the current bank no longer has.
    const bool fits =
        output.size() >= rows && offset <= bank.size() && rows <= bank.size() - offset;
    if (fits) {
        std::copy_n(bank.begin() + static_cast<std::ptrdiff_t>(offset), rows, output.begin());
        count = rows;
    }
    return fits;
}

/** Copies every stem row in ascending name order. */
bool snapshot(std::span<Stem> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_stems.snapshot(output, count);
}

/** Copies the whole flat name-hash bank. */
bool snapshot_hashes(std::span<NameHash> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_nameHashes.snapshot(output, count);
}

/** @return The stem row count, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_stems.count();
}

/** @return The name-hash row count, read under the lock. */
std::size_t hash_count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_nameHashes.count();
}

} // namespace sunrise::state::build_data::spawn_sets
