#include <algorithm>
#include <cstring>
#include <limits>

#include "../../../../core/filesystem/path.h"
#include "../../../crypto/sha1.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::config_bake::internal {
namespace {

struct Range final {
    std::uint64_t first{}, end{};
};

/** @param bytes Region bytes. @param expected Stored SHA-1. @return True on an exact digest. */
[[nodiscard]] bool digest(std::span<const std::byte> bytes,
                          std::span<const std::byte> expected) noexcept {
    crypto::sha1::Digest actual{};
    return crypto::sha1::hash(bytes, actual)
           && std::equal(actual.begin(), actual.end(), expected.begin(), expected.end());
}

/**
 * Metadata table descriptors must agree with the bounded physical tables.
 * @param package Parsed regions and capacities.
 * @return False when a descriptor disagrees or its tail cannot be relocated.
 */
[[nodiscard]] bool metadata_valid(const tables::Package& package) noexcept {
    const auto& data = package.metadata;
    if (load<std::uint64_t>(data, MetadataField::kTotalSize) != data.size()
        || load<std::uint64_t>(data, MetadataField::kEntryCount) != package.entryCount
        || load<std::uint64_t>(data, MetadataField::kBlockCount) != package.blockCount
        || load<std::uint64_t>(data, MetadataField::kEntryPointer)
               != MetadataField::kEntryTable - MetadataField::kEntryPointer
                      - tables::ArrayPrefixField::kCapacity
        || load<std::uint64_t>(data, MetadataField::kBlockPointer)
               != package.blockTable - MetadataField::kBlockPointer
                      - tables::ArrayPrefixField::kCapacity) {
        return false;
    }
    const auto count = load<std::uint64_t>(data, MetadataField::kTagPairCount);
    if (count == 0) {
        return true;
    }
    const auto relative = load<std::int64_t>(data, MetadataField::kTagPairPointer);
    /** The field-relative pointer targets sixteen bytes before the first row. */
    constexpr auto adjustment =
        MetadataField::kTagPairPointer + tables::ArrayPrefixField::kCapacity;
    if (relative < 0 || static_cast<std::uint64_t>(relative) > data.size() - adjustment) {
        return false;
    }
    const std::uint64_t first = static_cast<std::uint64_t>(relative) + adjustment;
    const std::uint64_t tail =
        package.blockTable + package.blockCapacity * layout::kBlockRecordSize;
    return first >= tail + layout::kTableArrayPrefixSize && first < data.size()
           && load<std::uint32_t>(data, first - tables::ArrayPrefixField::kMarker)
                  == layout::kTableArrayMarker
           && load<std::uint64_t>(data, first - tables::ArrayPrefixField::kCapacity) >= count
           && count <= data.size() - first;
}

/**
 * Each local block is bounded and hashed without decoding or requesting any key.
 * @param input Held file and parsed regions.
 * @return The first layout, read or digest failure.
 */
[[nodiscard]] Status verify_file(Input& input) {
    const auto& package = input.package;
    const auto& header = package.header;
    const auto footer = load<std::uint32_t>(header, HeaderField::kFooterOffset);
    if (load<std::uint32_t>(header, HeaderField::kFileSize) != input.size
        || footer % kDiskAlignment != 0 || std::uint64_t{footer} + kDiskAlignment != input.size
        || !metadata_valid(package)) {
        return Status::format;
    }
    std::array<std::byte, sizeof kFooterMarker> marker{};
    if (!tables::read_exact(input.file.get(), footer, marker)
        || load<std::uint32_t>(marker, 0) != kFooterMarker) {
        return Status::format;
    }
    if (!digest(package.misc,
                std::span(header).subspan(HeaderField::kMiscDigest, crypto::sha1::kDigestSize))
        || !digest(
            package.metadata,
            std::span(header).subspan(HeaderField::kMetadataDigest, crypto::sha1::kDigestSize))) {
        return Status::source;
    }
    std::vector<Range> ranges{{0, layout::kHeaderSize}, {footer, input.size}};
    const auto signature = load<std::uint32_t>(header, HeaderField::kSignatureOffset);
    if (signature != 0) {
        ranges.push_back({signature, signature + kDiskAlignment});
    }
    for (const auto field : {HeaderField::kMiscOffset, HeaderField::kMetadataOffset}) {
        const auto first = load<std::uint32_t>(header, field);
        const auto count = load<std::uint32_t>(header, field + sizeof(std::uint32_t));
        if (count != 0) {
            ranges.push_back({first, std::uint64_t{first} + count});
        }
    }
    std::vector<std::byte> bytes;
    for (std::uint32_t index = 0; index < package.blockCount; ++index) {
        layout::BlockRecord record{};
        std::memcpy(&record, tables::block_bytes(package, index).data(), sizeof record);
        if (record.size == 0 || record.size > layout::kBlockSize
            || record.patchId > package.patchId) {
            return Status::blocks;
        }
        if (record.patchId != package.patchId) {
            continue;
        }
        const std::uint64_t end = std::uint64_t{record.offset} + record.size;
        if (end > footer) {
            return Status::blocks;
        }
        ranges.push_back({record.offset, end});
        bytes.resize(record.size);
        if (!tables::read_exact(input.file.get(), record.offset, bytes)) {
            return Status::unreadable;
        }
        if (!digest(bytes, record.sha1)) {
            return Status::source;
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](Range a, Range b) { return a.first < b.first; });
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].end > input.size || (i != 0 && ranges[i - 1].end > ranges[i].first)) {
            return Status::format;
        }
    }
    return Status::ok;
}

/**
 * A source cannot be replaced or opened for writing while its read handle remains owned.
 * @param path Raw package path.
 * @param input Receives the handle and checked regions.
 * @return The first read, format or digest failure.
 */
[[nodiscard]] Status open(const wchar_t* path, Input& input) {
    if (path == nullptr || *path == L'\0') {
        return Status::unreadable;
    }
    input.file = File(CreateFileW(core::path::extended(path).c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (!input.file.valid()) {
        return Status::unreadable;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(input.file.get(), &size) == FALSE || size.QuadPart < 0) {
        return Status::unreadable;
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > (std::numeric_limits<std::uint32_t>::max)()) {
        return Status::capacity;
    }
    input.size = static_cast<std::uint32_t>(size.QuadPart);
    const auto status = tables::read_package(input.file.get(), input.package);
    if (status != merge::Status::ok) {
        return status == merge::Status::unreadable ? Status::unreadable : Status::format;
    }
    return verify_file(input);
}

} // namespace

/** Holds every declared ancestor and verifies its block-record identity against the newest file. */
Status read_inputs(const wchar_t* source, std::span<const Parent> parents, Prepared& output) {
    if (parents.size() > tables::kPatchLimit) {
        return Status::capacity;
    }
    Status status = open(source, output.source);
    if (status != Status::ok) {
        return status;
    }
    auto& newest = output.source.package;
    output.generations[newest.patchId] = &output.source;
    output.parents.resize(parents.size());
    for (std::size_t i = 0; i < parents.size(); ++i) {
        const auto& declared = parents[i];
        if (declared.patchId >= newest.patchId || output.generations[declared.patchId] != nullptr) {
            return Status::identity;
        }
        auto& input = output.parents[i];
        status = open(declared.path.c_str(), input);
        if (status != Status::ok) {
            return status;
        }
        const auto& prior = input.package;
        if (prior.patchId != declared.patchId || prior.packageId != newest.packageId
            || load<std::uint64_t>(prior.header, HeaderField::kBuildSignature)
                   != load<std::uint64_t>(newest.header, HeaderField::kBuildSignature)
            || prior.blockCount > newest.blockCount || prior.entryCount > newest.entryCount) {
            return Status::identity;
        }
        for (std::uint32_t block = 0; block < prior.blockCount; ++block) {
            const auto before = tables::block_bytes(prior, block);
            const auto current = tables::block_bytes(newest, block);
            if (!std::equal(before.begin(), before.end(), current.begin())) {
                return Status::identity;
            }
        }
        output.generations[prior.patchId] = &input;
    }
    return Status::ok;
}

/** Every fragment is read from its recorded generation; absent raw ancestors are refused. */
Status read_entry(const Prepared& plan, std::uint32_t entryIndex, std::vector<std::byte>& bytes) {
    const auto& source = plan.source.package;
    if (entryIndex >= source.entryCount) {
        return Status::identity;
    }
    layout::EntryRecord record{};
    std::memcpy(&record, tables::entry_bytes(source, entryIndex).data(), sizeof record);
    const auto place = layout::placement(record);
    const std::uint64_t end = std::uint64_t{place.startOffset} + place.size;
    if (place.size == 0 || place.size > state::mods::kConfigEntryByteLimit
        || place.startBlock + (end - 1) / layout::kBlockSize >= source.blockCount) {
        return Status::blocks;
    }
    bytes.resize(place.size);
    std::size_t copied = 0;
    std::uint32_t index = place.startBlock, offset = place.startOffset;
    while (copied < bytes.size()) {
        layout::BlockRecord block{};
        std::memcpy(&block, tables::block_bytes(source, index).data(), sizeof block);
        if (block.flags != 0 || block.patchId >= plan.generations.size()
            || plan.generations[block.patchId] == nullptr || offset >= block.size) {
            return Status::blocks;
        }
        const auto& owner = *plan.generations[block.patchId];
        if (index >= owner.package.blockCount
            || !std::equal(tables::block_bytes(source, index).begin(),
                           tables::block_bytes(source, index).end(),
                           tables::block_bytes(owner.package, index).begin())) {
            return Status::identity;
        }
        const std::size_t take = (std::min)(bytes.size() - copied, layout::kBlockSize - offset);
        if (take > block.size - offset
            || !tables::read_exact(owner.file.get(),
                                   std::uint64_t{block.offset} + offset,
                                   std::span(bytes).subspan(copied, take))) {
            return Status::blocks;
        }
        copied += take;
        ++index;
        offset = 0;
    }
    return Status::ok;
}

} // namespace sunrise::middleware::content::packages::config_bake::internal
