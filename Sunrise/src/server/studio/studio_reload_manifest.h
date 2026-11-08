#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::server::studio::reload_manifest {

/**
 * The tag list a reloadTags, patchTags or queryResident command names. Studio creates the
 * mapping before it writes the request id; the game writes one Result per Entry.
 */
inline constexpr std::uint32_t kMagic = 0x464D5353; // "SSMF"
inline constexpr std::uint16_t kVersion = 1;
/** Studio's process id, then the request id it answers. */
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Manifest.%lu.%lu";
inline constexpr std::size_t kMaximumBytes = std::size_t{16} << 20;
inline constexpr std::uint32_t kMaximumEntries = 65'535;

/** Studio writes the edit kind; the game does not read it. */
enum class EditKind : std::uint8_t { none, patch, replace, added, removed };

/** Entry flag bits; poke-safe ranges hold only scalars, spawn-time tags are read at spawn. */
inline constexpr std::uint8_t kLayoutPreserving = 1, kPokeSafe = 2, kSpawnTime = 4;

enum class Outcome : std::uint8_t {
    none,
    applied,
    nextSpawn,
    needsBubbleReload,
    refused,
    guardMismatch,
    notResident,
    /** Answers queryResident; the detail is the crc32 of the resident bytes. */
    resident,
    /** The client reads its package list once, so only a restart loads the tag. */
    needsRestart,
};

/** Detail codes a refused entry carries. */
enum class Detail : std::uint32_t {
    none,
    /** The mod's new files did not stage; the previous generation stays live. */
    stageFailed,
    /** The tag is not in the package set the reload published. */
    tagMissing,
    /** The patch ranges are malformed or run outside the tag. */
    patchUnsupported,
    /** The resident slot holds another class. */
    classMismatch,
    /** The write faulted. */
    writeFailed,
};

struct ManifestHeader final {
    std::uint32_t magic{};
    std::uint16_t version{};
    /** Studio writes it; the game does not read it. */
    std::uint16_t flags{};
    std::uint32_t request{};
    std::uint32_t entryCount{};
    /** Studio writes it; the game does not read it. */
    std::uint32_t generation{};
    std::uint32_t patchBytes{};
    std::uint64_t entriesOffset{};
    std::uint64_t patchOffset{};
    std::uint64_t resultsOffset{};
    std::array<std::uint8_t, 16> reserved{};
};

struct Entry final {
    std::uint32_t tag{};
    std::uint32_t classId{};
    EditKind kind{};
    std::uint8_t flags{};
    std::uint16_t patchRanges{};
    std::uint32_t firstPatch{};
    std::uint32_t newSize{};
    std::uint32_t packageId{};
    std::uint32_t entryIndex{};
    std::uint32_t reserved{};
};

/** Followed by `length` new bytes, padded to 4. The crc32 guards the bytes being replaced. */
struct PatchRange final {
    std::uint32_t offset{};
    std::uint32_t length{};
    std::uint32_t oldCrc32{};
};

struct Result final {
    Outcome outcome{};
    std::uint8_t tier{};
    std::uint16_t reserved{};
    /** A Detail, or the resident crc32 for Outcome::resident. */
    std::uint32_t detail{};
};

namespace layout {
/** Studio writes the mapping at these same offsets. */
inline constexpr std::size_t kHeaderBytes = 64, kEntryBytes = 32, kPatchRangeBytes = 12;
inline constexpr std::size_t kResultBytes = 8;
inline constexpr std::size_t kHeaderVersion = 0x04, kHeaderFlags = 0x06, kHeaderRequest = 0x08;
inline constexpr std::size_t kHeaderCount = 0x0C, kHeaderGeneration = 0x10,
                             kHeaderPatchBytes = 0x14;
inline constexpr std::size_t kHeaderEntries = 0x18, kHeaderPatches = 0x20, kHeaderResults = 0x28;
inline constexpr std::size_t kHeaderReserved = 0x30;
inline constexpr std::size_t kEntryKind = 0x08, kEntryFlags = 0x09, kEntryPatchRanges = 0x0A;
inline constexpr std::size_t kEntryFirstPatch = 0x0C, kEntryNewSize = 0x10, kEntryPackage = 0x14;
inline constexpr std::size_t kEntryIndex = 0x18;
inline constexpr std::size_t kResultTier = 1, kResultDetail = 4;
} // namespace layout

static_assert(sizeof(ManifestHeader) == layout::kHeaderBytes);
static_assert(offsetof(ManifestHeader, version) == layout::kHeaderVersion);
static_assert(offsetof(ManifestHeader, flags) == layout::kHeaderFlags);
static_assert(offsetof(ManifestHeader, request) == layout::kHeaderRequest);
static_assert(offsetof(ManifestHeader, entryCount) == layout::kHeaderCount);
static_assert(offsetof(ManifestHeader, generation) == layout::kHeaderGeneration);
static_assert(offsetof(ManifestHeader, patchBytes) == layout::kHeaderPatchBytes);
static_assert(offsetof(ManifestHeader, entriesOffset) == layout::kHeaderEntries);
static_assert(offsetof(ManifestHeader, patchOffset) == layout::kHeaderPatches);
static_assert(offsetof(ManifestHeader, resultsOffset) == layout::kHeaderResults);
static_assert(offsetof(ManifestHeader, reserved) == layout::kHeaderReserved);
static_assert(sizeof(Entry) == layout::kEntryBytes);
static_assert(offsetof(Entry, kind) == layout::kEntryKind);
static_assert(offsetof(Entry, flags) == layout::kEntryFlags);
static_assert(offsetof(Entry, patchRanges) == layout::kEntryPatchRanges);
static_assert(offsetof(Entry, firstPatch) == layout::kEntryFirstPatch);
static_assert(offsetof(Entry, newSize) == layout::kEntryNewSize);
static_assert(offsetof(Entry, packageId) == layout::kEntryPackage);
static_assert(offsetof(Entry, entryIndex) == layout::kEntryIndex);
static_assert(sizeof(PatchRange) == layout::kPatchRangeBytes);
static_assert(sizeof(Result) == layout::kResultBytes);
static_assert(offsetof(Result, tier) == layout::kResultTier);
static_assert(offsetof(Result, detail) == layout::kResultDetail);

} // namespace sunrise::server::studio::reload_manifest
