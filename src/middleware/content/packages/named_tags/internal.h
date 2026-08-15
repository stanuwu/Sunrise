#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../named_tags.h"

namespace sunrise::middleware::content::packages::named_tags {

/** One directory walk's visitor, wrapped so every entry carries its file's patch index. */
struct PatchStamp {
    Visitor inner{};
    void* context{};
    std::uint32_t patchIndex{};
};

/**
 * Reads the patch index out of one package leaf name.
 * @param fileName Whole leaf name, ending in an underscore, digits and the extension.
 * @return The patch index, or zero when the name does not have that form.
 */
[[nodiscard]] std::uint32_t leaf_patch_index(const wchar_t* fileName) noexcept;

/**
 * Forwards one entry to the wrapped visitor with its patch index filled in.
 * @param context The stamp.
 * @param entry Entry as the parser produced it.
 * @return Whatever the wrapped visitor returns.
 */
[[nodiscard]] bool stamp_patch(void* context, const Entry& entry) noexcept;

/** Random-access reader used by file and fixed-vector package sources. */
struct Reader {
    using Function = bool (*)(void* context,
                              std::uint64_t offset,
                              std::span<std::byte> output) noexcept;

    void* context{};
    std::uint64_t size{};
    Function read{};
};

/** Supported package-directory variants. Each uses different named-table fields. */
enum class Variant : std::uint8_t {
    beta = 0,
    preBeyondLight = 1,
};

#pragma pack(push, 1)
/** Header prefix through the package misc-data location. */
struct PackageHeader {
    std::uint16_t version{};
    std::uint16_t platform{};
    std::uint16_t packageId{};
    std::array<std::byte, 20> beforeVariant{};
    Variant variant{};
    std::array<std::byte, 209> beforeMisc{};
    std::uint32_t miscSize{};
    std::uint32_t miscOffset{};
};

/** Self-relative array field whose offset anchor follows its reserved word. */
struct RelativeDirectory {
    std::uint64_t count{};
    std::uint64_t relativeOffset{};
    std::uint64_t reservedAnchor{};
};

/** Beta misc prefix before the named-definition directory. */
struct BetaMiscDirectory {
    std::array<std::byte, 8> prefix{};
    RelativeDirectory namedTags{};
};

/** Pre-Beyond-Light misc prefix before the named-definition directory. */
struct PreBeyondLightMiscDirectory {
    std::array<std::byte, 16> prefix{};
    RelativeDirectory namedTags{};
};

/** One fixed named-definition table record. */
struct Record {
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::uint64_t nameRelativeOffset{};
};
#pragma pack(pop)

/** Package format version 38 is the only supported installed container layout. */
inline constexpr std::uint16_t kSupportedVersion = 38;
/** Per-package cap. It rejects corrupt counts before the table size is multiplied out. */
inline constexpr std::uint64_t kEntryCountLimit = 8192;
/** Packed package header prefix ends after the misc-data location. */
inline constexpr std::size_t kPackageHeaderSize = 244;
/** Relative array fields hold count, offset, and offset-anchor words. */
inline constexpr std::size_t kRelativeDirectorySize = 24;
/** Named-definition records hold two ids and one relative name offset. */
inline constexpr std::size_t kRecordSize = 16;

/**
 * Parses one random-access package and visits every named definition.
 * It keeps no pointers into the source.
 * @param reader Bounded package reader.
 * @param visitor Required entry consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives parsed entry totals.
 * @return True when every header, range, name, and visitor call is valid.
 */
[[nodiscard]] bool
extract(const Reader& reader, Visitor visitor, void* context, Result& result) noexcept;

} // namespace sunrise::middleware::content::packages::named_tags
