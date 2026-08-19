#include "custom_package_builder.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../client/content/items/packages/internal.h"
#include "../../core/logging/log.h"
#include "../../middleware/compression/oodle/runtime.h"
#include "../../middleware/content/packages/reader/internal.h"
#include "../../middleware/content/packages/reader/reader.h"
#include "../../middleware/content/packages/tables/definition_index_table.h"
#include "../../middleware/crypto/aes_gcm_decrypt.h"
#include "../../middleware/crypto/aes_gcm_encrypt.h"
#include "../../middleware/crypto/sha1.h"
#include "../../state/content/content_catalog.h"

namespace sunrise::izanami::runtime::custom_package_builder {
namespace {

namespace compression = middleware::compression::oodle;
namespace crypto = middleware::crypto::aes_gcm;
namespace sha1 = middleware::crypto::sha1;
namespace item_packages = client::content::items::packages;
namespace reader = middleware::content::packages::reader;
namespace layout = middleware::content::packages::reader::layout;
namespace tables = middleware::content::packages::tables;

constexpr std::size_t kFileAlignment = 0x1000;
constexpr std::size_t kHeaderFileSizeOffset = 0x164;
constexpr std::byte kNonceBranchByte{0xF9};
constexpr std::uint64_t kMaximumPatchBytes = 128ULL * 1024ULL * 1024ULL;

struct StaticTableRedirect {
    std::uint32_t tableTag{};
    std::uint32_t originalResourceTag{};
};

/** The tiny six-instance static map that forms the VFX test baseplate. */
constexpr std::uint32_t kPandoraBaseplateStaticTable = 0x8150E15BU;
constexpr std::uint32_t kPandoraBaseplateStaticResource = 0x8150E15AU;
/** The two larger first-bubble scenery tables, and their original static-map parents. */
constexpr std::array kPandoraStaticTableRedirects{
    StaticTableRedirect{0x8150E018U, 0x8150E017U},
    StaticTableRedirect{0x8150E14EU, 0x8150E14DU},
};

/** Fixed fields and offsets needed to rebuild one latest-patch file. */
struct PackageFile {
    std::vector<std::byte> bytes{};
    reader::Path stem{};
    reader::Path latestPath{};
    std::uint64_t entryTable{};
    std::uint64_t blockTable{};
    std::uint32_t entryCount{};
    std::uint32_t blockCount{};
    std::uint32_t latestPatch{};
    std::uint16_t packageId{};
};

/** One decoded package block carrying one or more staged entry mutations. */
struct MutableBlock {
    std::uint32_t index{};
    std::vector<std::byte> decoded{};
    layout::BlockRecord record{};
};

/** Reports one package-authoring stage. */
void report(std::string_view stage,
            std::string_view result,
            std::uint16_t packageId = 0,
            std::uint32_t patch = 0,
            std::uint32_t block = 0,
            std::size_t bytes = 0) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=izanami_package_build stage=%.*s result=%.*s package=0x%04X patch=%u block=%u "
        "bytes=%zu",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<unsigned>(packageId),
        static_cast<unsigned>(patch),
        static_cast<unsigned>(block),
        bytes);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == "ok" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> bytes, std::size_t offset, Value& value) noexcept {
    value = {};
    if (offset > bytes.size() || bytes.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return true;
}

template <typename Value>
[[nodiscard]] bool
write_value(std::span<std::byte> bytes, std::size_t offset, const Value& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(bytes.data() + offset, &value, sizeof value);
    return true;
}

/** Reads an entire bounded package patch file. */
[[nodiscard]] bool read_file(const reader::Path& path, std::vector<std::byte>& output) noexcept {
    output.clear();
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    const bool sized = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0
                       && static_cast<std::uint64_t>(size.QuadPart) <= kMaximumPatchBytes;
    if (!sized) {
        CloseHandle(file);
        return false;
    }
    output.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool complete =
        ReadFile(file, output.data(), static_cast<DWORD>(output.size()), &read, nullptr) != FALSE
        && read == output.size();
    CloseHandle(file);
    if (!complete) {
        output.clear();
    }
    return complete;
}

/** Writes a complete staged file and flushes it before reporting success. */
[[nodiscard]] bool write_file(const std::wstring& path, std::span<const std::byte> bytes) noexcept {
    if (bytes.empty() || bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool complete =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE
        && written == bytes.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return complete;
}

/** Loads and structurally validates the latest patch header and tables. */
[[nodiscard]] bool
load_package(std::wstring_view directory, std::uint16_t packageId, PackageFile& output) noexcept {
    output = {};
    output.packageId = packageId;
    if (!reader::find_latest(directory, packageId, output.stem, output.latestPatch)
        || output.latestPatch == (std::numeric_limits<std::uint16_t>::max)()
        || !reader::build_path(output.stem, output.latestPatch, output.latestPath)
        || !read_file(output.latestPath, output.bytes)
        || output.bytes.size() < layout::kHeaderSize) {
        return false;
    }
    std::uint16_t version = 0;
    std::uint32_t entryTableRelative = 0;
    if (!read_value(output.bytes, layout::HeaderOffsets::kVersion, version)
        || version != layout::kSupportedVersion
        || !read_value(output.bytes, layout::HeaderOffsets::kEntryCount, output.entryCount)
        || !read_value(output.bytes, layout::HeaderOffsets::kBlockCount, output.blockCount)
        || !read_value(output.bytes, layout::HeaderOffsets::kEntryTable, entryTableRelative)) {
        return false;
    }
    output.entryTable =
        static_cast<std::uint64_t>(entryTableRelative) + layout::kEntryTableAdjustment;
    output.blockTable =
        output.entryTable
        + static_cast<std::uint64_t>(output.entryCount) * sizeof(layout::EntryRecord)
        + layout::kBlockTableGap;
    const std::uint64_t tableEnd =
        output.blockTable
        + static_cast<std::uint64_t>(output.blockCount) * sizeof(layout::BlockRecord);
    return output.entryCount != 0 && output.blockCount != 0 && tableEnd <= output.bytes.size();
}

/** Builds the package-wide nonce used by every encrypted block. */
[[nodiscard]] std::array<std::byte, crypto::kNonceSize>
package_nonce(const reader::BlockKeys& keys, std::uint16_t packageId) noexcept {
    std::array<std::byte, crypto::kNonceSize> nonce = keys.nonceBase;
    nonce[0] ^= static_cast<std::byte>((packageId >> 8U) & 0xFFU);
    nonce[1] = kNonceBranchByte;
    nonce[11] ^= static_cast<std::byte>(packageId & 0xFFU);
    return nonce;
}

/** Compresses and encrypts one decoded block according to its existing table flags. */
[[nodiscard]] bool encode_block(std::span<const std::byte> decoded,
                                const reader::BlockKeys& keys,
                                std::uint16_t packageId,
                                layout::BlockRecord& record,
                                std::vector<std::byte>& stored) noexcept {
    stored.clear();
    std::vector<std::byte> compressed{};
    std::span<const std::byte> encoded = decoded;
    const HMODULE oodle = GetModuleHandleW(L"oo2core_3_win64.dll");
    if ((record.flags & layout::BlockFlags::kCompressed) != 0) {
        std::size_t capacity = 0;
        std::size_t written = 0;
        if (oodle == nullptr || !compression::required_capacity(oodle, decoded.size(), capacity)) {
            return false;
        }
        compressed.resize(capacity);
        if (!compression::compress(oodle, decoded, compressed, written)) {
            return false;
        }
        compressed.resize(written);
        encoded = compressed;
    }
    if ((record.flags & layout::BlockFlags::kEncrypted) == 0) {
        stored.assign(encoded.begin(), encoded.end());
        return !stored.empty();
    }
    stored.resize(encoded.size());
    const auto nonce = package_nonce(keys, packageId);
    const auto& key =
        (record.flags & layout::BlockFlags::kAlternateKey) != 0 ? keys.alternate : keys.primary;
    return crypto::encrypt(std::span<const std::byte, crypto::kKeySize>(key),
                           nonce,
                           encoded,
                           stored,
                           std::span<std::byte, crypto::kTagSize>(record.tag));
}

/** Decodes the newly generated body and proves it exactly reproduces the source block. */
[[nodiscard]] bool validate_block(std::span<const std::byte> stored,
                                  std::span<const std::byte> expected,
                                  const reader::BlockKeys& keys,
                                  std::uint16_t packageId,
                                  const layout::BlockRecord& record) noexcept {
    std::vector<std::byte> plaintext{};
    std::span<const std::byte> encoded = stored;
    if ((record.flags & layout::BlockFlags::kEncrypted) != 0) {
        plaintext.resize(stored.size());
        const auto nonce = package_nonce(keys, packageId);
        const auto& key =
            (record.flags & layout::BlockFlags::kAlternateKey) != 0 ? keys.alternate : keys.primary;
        if (!crypto::decrypt(std::span<const std::byte, crypto::kKeySize>(key),
                             nonce,
                             stored,
                             std::span<const std::byte, crypto::kTagSize>(record.tag),
                             plaintext)) {
            return false;
        }
        encoded = plaintext;
    }
    if ((record.flags & layout::BlockFlags::kCompressed) == 0) {
        return std::ranges::equal(encoded, expected);
    }
    std::vector<std::byte> decoded(expected.size());
    const HMODULE oodle = GetModuleHandleW(L"oo2core_3_win64.dll");
    return oodle != nullptr && compression::decompress(oodle, encoded, decoded)
           && std::ranges::equal(decoded, expected);
}

/** Rounds one file offset up to package block alignment. */
[[nodiscard]] constexpr std::size_t aligned(std::size_t value) noexcept {
    return (value + kFileAlignment - 1) & ~(kFileAlignment - 1);
}

/** Returns one mutable, single-block entry while sharing decoded blocks across mutations. */
[[nodiscard]] bool mutable_entry(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 const PackageFile& package,
                                 std::uint32_t tag,
                                 std::vector<MutableBlock>& blocks,
                                 std::span<std::byte>& bytes,
                                 std::uint32_t& classId) noexcept {
    bytes = {};
    classId = 0;
    if (tag < layout::kTagBase
        || static_cast<std::uint16_t>((tag - layout::kTagBase) >> layout::kTagEntryBits)
               != package.packageId) {
        return false;
    }
    const std::uint32_t entryIndex = (tag - layout::kTagBase) & layout::kTagEntryMask;
    layout::EntryRecord entry{};
    if (entryIndex >= package.entryCount
        || !read_value(package.bytes,
                       package.entryTable + static_cast<std::uint64_t>(entryIndex) * sizeof entry,
                       entry)) {
        return false;
    }
    classId = entry.reference;
    const layout::EntryPlacement placement = layout::placement(entry);
    if (placement.size == 0 || placement.startBlock >= package.blockCount
        || placement.startOffset > layout::kBlockSize
        || placement.size > layout::kBlockSize - placement.startOffset) {
        return false;
    }

    auto found = std::find_if(blocks.begin(), blocks.end(), [placement](const MutableBlock& row) {
        return row.index == placement.startBlock;
    });
    if (found == blocks.end()) {
        MutableBlock block{};
        block.index = placement.startBlock;
        if (!reader::read_block(
                source, scratch, package.packageId, block.index, block.decoded, block.record)) {
            return false;
        }
        blocks.push_back(std::move(block));
        found = std::prev(blocks.end());
    }
    if (placement.startOffset > found->decoded.size()
        || placement.size > found->decoded.size() - placement.startOffset) {
        return false;
    }
    bytes = std::span(found->decoded).subspan(placement.startOffset, placement.size);
    return true;
}

/** Opens one single-entry Shadowkeep static-map table and validates its inline resource shape. */
[[nodiscard]] bool static_map_table(const reader::Source& source,
                                    reader::Scratch& scratch,
                                    const PackageFile& package,
                                    std::uint32_t tableTag,
                                    std::vector<MutableBlock>& blocks,
                                    std::span<std::byte>& table,
                                    std::uint32_t& resourceTag) noexcept {
    constexpr std::uint32_t kMapDataTableClass = 0x808099D6;
    constexpr std::uint32_t kMapDataEntryClass = 0x808099D8;
    constexpr std::uint32_t kMapDataResourceClass = 0x808071B3;
    constexpr std::size_t kTableSize = 0xE0;
    constexpr std::size_t kFileSizeOffset = 0;
    constexpr std::size_t kPlacementCountOffset = 0x8;
    constexpr std::size_t kPlacementPointerOffset = 0x10;
    constexpr std::size_t kEntryClassOffset = 0x28;
    constexpr std::size_t kResourceClassOffset = 0xC4;
    constexpr std::size_t kResourceTagOffset = 0xD8;
    constexpr std::int64_t kInlinePlacementPointer = 0x10;

    table = {};
    resourceTag = 0;
    std::uint32_t classId = 0;
    if (!mutable_entry(source, scratch, package, tableTag, blocks, table, classId)
        || classId != kMapDataTableClass || table.size() != kTableSize) {
        return false;
    }
    std::uint64_t fileSize = 0;
    std::uint32_t count = 0;
    std::int64_t placementPointer = 0;
    std::uint32_t entryClass = 0;
    std::uint32_t resourceClass = 0;
    return read_value(table, kFileSizeOffset, fileSize) && fileSize == table.size()
           && read_value(table, kPlacementCountOffset, count) && count == 1
           && read_value(table, kPlacementPointerOffset, placementPointer)
           && placementPointer == kInlinePlacementPointer
           && read_value(table, kEntryClassOffset, entryClass) && entryClass == kMapDataEntryClass
           && read_value(table, kResourceClassOffset, resourceClass)
           && resourceClass == kMapDataResourceClass
           && read_value(table, kResourceTagOffset, resourceTag) && resourceTag >= layout::kTagBase;
}

/** Encodes every changed block into one next-patch clone and updates its public block table. */
[[nodiscard]] bool append_modified_blocks(PackageFile& package,
                                          std::span<MutableBlock> blocks,
                                          const reader::BlockKeys& keys,
                                          std::uint32_t newPatch) noexcept {
    if (blocks.empty() || newPatch > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    for (MutableBlock& block : blocks) {
        // The native package loader rejects freshly generated Oodle streams in this build.
        // Version 38 supports raw encrypted blocks, which preserve the decoded block exactly.
        block.record.flags &= static_cast<std::uint16_t>(~layout::BlockFlags::kCompressed);
        std::vector<std::byte> stored{};
        if (!encode_block(block.decoded, keys, package.packageId, block.record, stored)
            || !validate_block(stored, block.decoded, keys, package.packageId, block.record)) {
            return false;
        }
        sha1::Digest storedDigest{};
        if (!sha1::hash(stored, storedDigest)) {
            return false;
        }
        block.record.opaque = storedDigest;
        const std::size_t bodyOffset = aligned(package.bytes.size());
        package.bytes.resize(bodyOffset);
        package.bytes.insert(package.bytes.end(), stored.begin(), stored.end());
        if (bodyOffset > (std::numeric_limits<std::uint32_t>::max)()
            || stored.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        block.record.offset = static_cast<std::uint32_t>(bodyOffset);
        block.record.size = static_cast<std::uint32_t>(stored.size());
        block.record.patchId = static_cast<std::uint16_t>(newPatch);
        if (!write_value(package.bytes,
                         package.blockTable
                             + static_cast<std::uint64_t>(block.index) * sizeof block.record,
                         block.record)) {
            return false;
        }
    }
    package.bytes.resize(aligned(package.bytes.size()));
    if (package.bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    const std::uint16_t patchField = static_cast<std::uint16_t>(newPatch);
    const std::uint32_t fileSize = static_cast<std::uint32_t>(package.bytes.size());
    return write_value(package.bytes, layout::HeaderOffsets::kPatchId, patchField)
           && write_value(package.bytes, kHeaderFileSizeOffset, fileSize);
}

} // namespace

/** Builds and validates a staged Pandora patch with its large scenery redirected to baseplate. */
bool stage_map_root(std::string_view rootName) noexcept {
    std::array<state::content::Definition, 4> matches{};
    std::size_t matchCount = 0;
    if (!state::content::lookup(rootName, matches, matchCount) || matchCount == 0) {
        report("resolve_root", "missing");
        return false;
    }
    const std::uint32_t rootTag = matches[0].tag;
    const std::uint16_t packageId = tables::package_of(rootTag);
    if (packageId == tables::kAbsentPackageId) {
        report("resolve_root", "bad_tag");
        return false;
    }

    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!item_packages::collect_keys(keys) || !item_packages::package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report("keys", "unavailable", packageId);
        return false;
    }
    const reader::Source source{directory.chars.data(), &keys};
    auto scratch = std::make_unique<reader::Scratch>();
    PackageFile package{};
    bool complete = scratch != nullptr && load_package(source.directory, packageId, package);
    std::vector<MutableBlock> blocks{};
    blocks.reserve(kPandoraStaticTableRedirects.size());
    if (complete) {
        std::span<std::byte> baseplateTable{};
        std::uint32_t baseplateResource = 0;
        complete = static_map_table(source,
                                    *scratch,
                                    package,
                                    kPandoraBaseplateStaticTable,
                                    blocks,
                                    baseplateTable,
                                    baseplateResource)
                   && baseplateResource == kPandoraBaseplateStaticResource;
    }
    if (complete) {
        constexpr std::size_t kResourceTagOffset = 0xD8;
        for (const StaticTableRedirect& redirect : kPandoraStaticTableRedirects) {
            std::span<std::byte> table{};
            std::uint32_t resourceTag = 0;
            if (!static_map_table(
                    source, *scratch, package, redirect.tableTag, blocks, table, resourceTag)
                || (resourceTag != redirect.originalResourceTag
                    && resourceTag != kPandoraBaseplateStaticResource)
                || !write_value(table, kResourceTagOffset, kPandoraBaseplateStaticResource)) {
                complete = false;
                break;
            }
        }
    }
    const std::uint32_t newPatch = package.latestPatch + 1;
    if (complete) {
        complete = append_modified_blocks(package, blocks, keys, newPatch);
    }

    std::wstring stagedPath{};
    if (complete) {
        reader::Path nextPath{};
        complete = reader::build_path(package.stem, newPatch, nextPath);
        if (complete) {
            stagedPath.assign(nextPath.chars.data());
            stagedPath += L".izanami-stage";
            complete = write_file(stagedPath, package.bytes);
        }
    }
    if (scratch != nullptr) {
        reader::close_files(*scratch);
    }
    SecureZeroMemory(&keys, sizeof keys);
    report("stage_baseplate_variant",
           complete ? "ok" : "fail",
           packageId,
           newPatch,
           static_cast<std::uint32_t>(blocks.size()),
           complete ? package.bytes.size() : 0);
    return complete;
}

} // namespace sunrise::izanami::runtime::custom_package_builder
