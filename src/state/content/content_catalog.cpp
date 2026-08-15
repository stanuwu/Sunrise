#include "content_catalog.h"

#include <Windows.h>

#include <algorithm>

namespace sunrise::state::content {
namespace {

/** Standard 64-bit FNV-1a offset basis starts each deterministic name hash. */
constexpr std::uint64_t kHashOffsetBasis = 14695981039346656037ULL;
/** Standard 64-bit FNV-1a prime mixes each extracted name byte. */
constexpr std::uint64_t kHashPrime = 1099511628211ULL;
/** Standard 32-bit FNV-1 offset basis starts each compact content-name id. */
constexpr std::uint32_t kContentHashOffsetBasis = 2166136261U;
/** Standard 32-bit FNV-1 prime mixes each extracted content-name byte. */
constexpr std::uint32_t kContentHashPrime = 16777619U;

/** One used or empty open-addressing catalog slot. */
struct Slot {
    Definition definition;
    std::uint32_t contentNameHash{};
    bool occupied{};
};

std::array<Slot, kDefinitionCatalogCapacity> g_slots{};
std::size_t g_count{};
bool g_sealed{};
SRWLOCK g_catalogLock{SRWLOCK_INIT};

static_assert((kDefinitionCatalogCapacity & (kDefinitionCatalogCapacity - 1)) == 0);

/** @param name UTF-8 definition name. @return Stable open-addressing start slot. */
[[nodiscard]] std::size_t hash_name(std::string_view name) noexcept {
    std::uint64_t hash = kHashOffsetBasis;
    for (const unsigned char value : name) {
        hash ^= value;
        hash *= kHashPrime;
    }
    return static_cast<std::size_t>(hash) & (kDefinitionCatalogCapacity - 1);
}

/** @param name Extracted ASCII name. @return Its compact 32-bit FNV-1 id. */
[[nodiscard]] std::uint32_t content_name_hash(std::string_view name) noexcept {
    std::uint32_t hash = kContentHashOffsetBasis;
    for (const unsigned char value : name) {
        hash *= kContentHashPrime;
        hash ^= value;
    }
    return hash;
}

/**
 * Compares one used slot with a complete extracted mapping.
 * @param slot Used catalog slot.
 * @param name Mapping name.
 * @param tag Mapping tag.
 * @param classId Mapping class.
 * @return True when all three mapping fields match.
 */
[[nodiscard]] bool
equals(const Slot& slot, std::string_view name, std::uint32_t tag, std::uint32_t classId) noexcept {
    const Definition& value = slot.definition;
    return value.tag == tag && value.classId == classId && value.nameLength == name.size()
           && std::equal(name.begin(), name.end(), value.name.begin());
}

/** @param slot Used catalog slot. @param name Name to compare. @return Exact name match. */
[[nodiscard]] bool name_equals(const Slot& slot, std::string_view name) noexcept {
    const Definition& value = slot.definition;
    return value.nameLength == name.size()
           && std::equal(name.begin(), name.end(), value.name.begin());
}

} // namespace

/** Clears every generated content mapping under the catalog lock. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_catalogLock);
    SecureZeroMemory(g_slots.data(), sizeof g_slots);
    g_count = 0;
    g_sealed = false;
    ReleaseSRWLockExclusive(&g_catalogLock);
}

/** Freezes the populated catalog against later extraction inserts. */
bool seal() noexcept {
    AcquireSRWLockExclusive(&g_catalogLock);
    g_sealed = g_count != 0;
    const bool result = g_sealed;
    ReleaseSRWLockExclusive(&g_catalogLock);
    return result;
}

/** Inserts one mapping with bounded linear probing, merging exact duplicates. */
bool insert(std::string_view name, std::uint32_t tag, std::uint32_t classId) noexcept {
    if (name.empty() || name.size() >= kDefinitionNameCapacity) {
        return false;
    }
    AcquireSRWLockExclusive(&g_catalogLock);
    if (g_sealed) {
        ReleaseSRWLockExclusive(&g_catalogLock);
        return false;
    }
    const std::size_t start = hash_name(name);
    for (std::size_t probe = 0; probe < g_slots.size(); ++probe) {
        Slot& slot = g_slots[(start + probe) & (g_slots.size() - 1)];
        if (slot.occupied && equals(slot, name, tag, classId)) {
            ReleaseSRWLockExclusive(&g_catalogLock);
            return true;
        }
        if (!slot.occupied) {
            std::copy(name.begin(), name.end(), slot.definition.name.begin());
            slot.definition.nameLength = name.size();
            slot.definition.tag = tag;
            slot.definition.classId = classId;
            slot.contentNameHash = content_name_hash(name);
            slot.occupied = true;
            ++g_count;
            ReleaseSRWLockExclusive(&g_catalogLock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_catalogLock);
    return false;
}

/** Copies every mapping for one name without handing out catalog storage. */
bool lookup(std::string_view name, std::span<Definition> output, std::size_t& count) noexcept {
    count = 0;
    if (name.empty()) {
        return true;
    }
    bool complete = true;
    AcquireSRWLockShared(&g_catalogLock);
    const std::size_t start = hash_name(name);
    for (std::size_t probe = 0; probe < g_slots.size(); ++probe) {
        const Slot& slot = g_slots[(start + probe) & (g_slots.size() - 1)];
        if (!slot.occupied) {
            break;
        }
        if (!name_equals(slot, name)) {
            continue;
        }
        if (count < output.size()) {
            output[count++] = slot.definition;
        } else {
            complete = false;
        }
    }
    ReleaseSRWLockShared(&g_catalogLock);
    return complete;
}

/** Copies every mapping picked by one compact extracted-name id. */
bool lookup_hash(std::uint32_t nameHash,
                 std::span<Definition> output,
                 std::size_t& count) noexcept {
    count = 0;
    bool complete = true;
    AcquireSRWLockShared(&g_catalogLock);
    for (const Slot& slot : g_slots) {
        if (!slot.occupied || slot.contentNameHash != nameHash) {
            continue;
        }
        if (count < output.size()) {
            output[count++] = slot.definition;
        } else {
            complete = false;
        }
    }
    ReleaseSRWLockShared(&g_catalogLock);
    return complete;
}

/** Copies each used mapping without handing out open-addressing storage. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    count = 0;
    AcquireSRWLockShared(&g_catalogLock);
    if (output.size() < g_count) {
        ReleaseSRWLockShared(&g_catalogLock);
        return false;
    }
    for (const Slot& slot : g_slots) {
        if (slot.occupied) {
            output[count++] = slot.definition;
        }
    }
    ReleaseSRWLockShared(&g_catalogLock);
    return true;
}

/** Rebuilds the whole catalog, and rolls back bad or duplicate cache rows. */
bool replace(std::span<const Definition> definitions) noexcept {
    clear();
    for (const Definition& definition : definitions) {
        if (definition.nameLength == 0 || definition.nameLength >= definition.name.size()
            || !insert(std::string_view(definition.name.data(), definition.nameLength),
                       definition.tag,
                       definition.classId)) {
            clear();
            return false;
        }
    }
    // Duplicate cache rows are rejected because a valid file stores distinct tuples.
    if (count() != definitions.size()) {
        clear();
        return false;
    }
    return true;
}

/** @return The count of distinct extracted mappings, read under the lock. */
std::size_t count() noexcept {
    AcquireSRWLockShared(&g_catalogLock);
    const std::size_t result = g_count;
    ReleaseSRWLockShared(&g_catalogLock);
    return result;
}

} // namespace sunrise::state::content
