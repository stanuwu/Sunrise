#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <tuple>
#include <vector>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/filesystem/temporary_sibling.h"
#include "../../../crypto/sha1.h"
#include "../table_merge/sidecar.h"
#include "internal.h"
#include "remap.h"

namespace sunrise::middleware::content::packages::merge::remap {
namespace {

namespace layout = reader::layout;
namespace sha1 = crypto::sha1;
namespace sidecar_layout = table_merge::layout;
using internal::BlockRecordField;
using internal::HeaderField;
using internal::Package;

// The copy is written under this suffix, then moved over the output.
constexpr std::wstring_view kTemporarySuffix = L".remap";
// Copies compare in chunks this large.
constexpr std::size_t kCompareChunk = std::size_t{1} << 20;

using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

/** @return The tag in package `to` with the same entry, when it sits in package `from`. */
[[nodiscard]] constexpr std::uint32_t
moved_tag(std::uint32_t tag, std::uint16_t from, std::uint16_t to) noexcept {
    return tag >= layout::kTagBase && layout::package_of(tag) == from
               ? layout::tag_of(to, tag & layout::kTagEntryMask)
               : tag;
}

// The move keeps the entry and changes only the package bits, and leaves other packages alone.
static_assert(moved_tag(0x80882005U, 0x0041, 0x0A10) == 0x81C20005U);
static_assert(moved_tag(0x80882005U, 0x0042, 0x0A10) == 0x80882005U);
static_assert(moved_tag(0x7FFFFFFFU, 0x0041, 0x0A10) == 0x7FFFFFFFU);

/** One file being copied: its tables, sidecar and the byte edits that land in its blocks. */
struct Item final {
    const File* file{};
    Package package;
    std::vector<std::byte> sidecar;
    bool hasSidecar{};
    bool miscChanged{};
    bool metadataChanged{};
    std::vector<std::pair<std::uint64_t, std::array<std::byte, 4>>> edits;
    std::vector<std::size_t> widths;
};

[[nodiscard]] Handle open_file(const std::wstring& path, DWORD access, DWORD disposition) noexcept {
    const HANDLE file = CreateFileW(core::path::extended(path.c_str()).c_str(),
                                    access,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    disposition,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    return {file == INVALID_HANDLE_VALUE ? nullptr : file, &CloseHandle};
}

[[nodiscard]] std::wstring sidecar_path(const std::wstring& package) {
    return package.substr(0, package.size() - layout::kPackageFileExtension.size())
        .append(table_merge::kSidecarFileExtension);
}

/** @return False on a read failure; a missing file leaves `present` false. */
[[nodiscard]] bool
read_whole(const std::wstring& path, std::vector<std::byte>& output, bool& present) {
    present = false;
    const Handle file = open_file(path, GENERIC_READ, OPEN_EXISTING);
    if (file == nullptr) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0
        || static_cast<std::uint64_t>(size.QuadPart) > table_merge::kSidecarByteLimit) {
        return false;
    }
    output.resize(static_cast<std::size_t>(size.QuadPart));
    present = true;
    return internal::read_exact(file.get(), 0, output);
}

/** @return True when both files hold the same bytes. */
[[nodiscard]] bool same_content(const std::wstring& first, const std::wstring& second) {
    const Handle left = open_file(first, GENERIC_READ, OPEN_EXISTING);
    const Handle right = open_file(second, GENERIC_READ, OPEN_EXISTING);
    LARGE_INTEGER leftSize{};
    LARGE_INTEGER rightSize{};
    if (left == nullptr || right == nullptr || GetFileSizeEx(left.get(), &leftSize) == FALSE
        || GetFileSizeEx(right.get(), &rightSize) == FALSE
        || leftSize.QuadPart != rightSize.QuadPart) {
        return false;
    }
    std::vector<std::byte> a(kCompareChunk);
    std::vector<std::byte> b(kCompareChunk);
    for (std::uint64_t at = 0; at < static_cast<std::uint64_t>(leftSize.QuadPart);) {
        const std::size_t count = static_cast<std::size_t>(
            (std::min)(static_cast<std::uint64_t>(kCompareChunk),
                       static_cast<std::uint64_t>(leftSize.QuadPart) - at));
        if (!internal::read_exact(left.get(), at, std::span(a).first(count))
            || !internal::read_exact(right.get(), at, std::span(b).first(count))
            || std::memcmp(a.data(), b.data(), count) != 0) {
            return false;
        }
        at += count;
    }
    return true;
}

/** @return True when the file is gone afterwards. */
bool remove_file(const std::wstring& path) noexcept {
    return DeleteFileW(core::path::extended(path.c_str()).c_str()) != FALSE
           || GetLastError() == ERROR_FILE_NOT_FOUND;
}

/** Keeps an identical output, so a copy the client holds open is never replaced. */
[[nodiscard]] bool publish(const std::wstring& temporary, const std::wstring& output) {
    if (same_content(temporary, output)) {
        return remove_file(temporary);
    }
    return core::path::publish_sibling(core::path::extended(temporary.c_str()).c_str(),
                                       core::path::extended(output.c_str()).c_str());
}

[[nodiscard]] bool write_whole(const std::wstring& path, std::span<const std::byte> bytes) {
    const Handle file = open_file(path, GENERIC_WRITE, CREATE_ALWAYS);
    return file != nullptr && internal::write_at(file.get(), 0, bytes);
}

[[nodiscard]] layout::BlockRecord block_record(const Package& package, std::uint32_t index) {
    layout::BlockRecord record{};
    std::memcpy(&record, internal::block_bytes(package, index).data(), sizeof record);
    return record;
}

/**
 * Finds the file and absolute byte that hold one entry offset, through the newest generation's
 * tables. The byte must sit in a plain block of one of the family's files.
 * @param holder Receives the item index of that file.
 * @return False when the offset leaves the entry, straddles blocks or sits in a coded block.
 */
[[nodiscard]] bool locate(std::span<const Item> items,
                          std::span<const std::size_t> family,
                          const Location& location,
                          std::size_t width,
                          std::size_t& holder,
                          std::uint32_t& block,
                          std::uint64_t& at) noexcept {
    const Package& newest = items[family.back()].package;
    if (location.entry >= newest.entryCount) {
        return false;
    }
    layout::EntryRecord entry{};
    std::memcpy(&entry, internal::entry_bytes(newest, location.entry).data(), sizeof entry);
    const layout::EntryPlacement placed = layout::placement(entry);
    std::uint64_t remaining = location.offset;
    std::uint32_t skip = placed.startOffset;
    for (block = placed.startBlock; block < newest.blockCount; ++block, skip = 0) {
        const layout::BlockRecord record = block_record(newest, block);
        if (record.size < skip) {
            return false;
        }
        if (remaining >= record.size - skip) {
            remaining -= record.size - skip;
            continue;
        }
        const std::uint64_t inBlock = skip + remaining;
        if (inBlock + width > record.size
            || (record.flags & (layout::BlockFlags::kCompressed | layout::BlockFlags::kEncrypted))
                   != 0) {
            return false;
        }
        const auto found = std::find_if(family.begin(), family.end(), [&](std::size_t index) {
            return items[index].package.patchId == record.patchId;
        });
        if (found == family.end()) {
            return false;
        }
        holder = *found;
        at = record.offset + inBlock;
        return true;
    }
    return false;
}

/** @return ok after every item's data edits land in its temporary copy. */
[[nodiscard]] Status copy_and_edit(std::span<Item> items) noexcept {
    for (const Item& item : items) {
        const std::wstring temporary = item.file->output + std::wstring(kTemporarySuffix);
        if (CopyFileW(core::path::extended(item.file->source.c_str()).c_str(),
                      core::path::extended(temporary.c_str()).c_str(),
                      FALSE)
            == FALSE) {
            return Status::writeFailed;
        }
        const Handle copy = open_file(temporary, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
        if (copy == nullptr) {
            return Status::writeFailed;
        }
        for (std::size_t index = 0; index < item.edits.size(); ++index) {
            const auto& [offset, bytes] = item.edits[index];
            if (!internal::write_at(
                    copy.get(), offset, std::span(bytes).first(item.widths[index]))) {
                return Status::writeFailed;
            }
        }
    }
    return Status::ok;
}

/** Writes the edited regions and the header into the temporary copy, then publishes it. */
[[nodiscard]] Status finish(Item& item, std::span<const Move> moves) noexcept {
    const std::wstring temporary = item.file->output + std::wstring(kTemporarySuffix);
    auto& header = item.package.header;
    for (const Move& move : moves) {
        if (move.from == item.package.packageId) {
            internal::store(std::span(header), HeaderField::kPackageId, move.to);
        }
    }
    sha1::Digest digest{};
    const auto misc = internal::load<std::uint32_t>(header, HeaderField::kMiscOffset);
    const auto metadata = internal::load<std::uint32_t>(header, HeaderField::kMetadataOffset);
    if (item.miscChanged) {
        if (!sha1::hash(item.package.misc, digest)) {
            return Status::writeFailed;
        }
        std::memcpy(header.data() + HeaderField::kMiscDigest, digest.data(), digest.size());
    }
    if (item.metadataChanged) {
        if (!sha1::hash(item.package.metadata, digest)) {
            return Status::writeFailed;
        }
        std::memcpy(header.data() + HeaderField::kMetadataDigest, digest.data(), digest.size());
    }
    {
        const Handle copy = open_file(temporary, GENERIC_WRITE, OPEN_EXISTING);
        if (copy == nullptr || !internal::write_at(copy.get(), 0, header)
            || (item.miscChanged && !internal::write_at(copy.get(), misc, item.package.misc))
            || (item.metadataChanged
                && !internal::write_at(copy.get(), metadata, item.package.metadata))) {
            return Status::writeFailed;
        }
    }
    if (!publish(temporary, item.file->output)) {
        return Status::writeFailed;
    }
    if (!item.hasSidecar) {
        return Status::ok;
    }
    if (item.sidecar.size()
        < sidecar_layout::kFileHeaderMetadataDigest + layout::kMetadataDigestSize) {
        return Status::format;
    }
    internal::store(std::span(item.sidecar),
                    sidecar_layout::kFileHeaderPackageId,
                    internal::load<std::uint16_t>(header, HeaderField::kPackageId));
    std::memcpy(item.sidecar.data() + sidecar_layout::kFileHeaderMetadataDigest,
                header.data() + HeaderField::kMetadataDigest,
                layout::kMetadataDigestSize);
    const std::wstring output = sidecar_path(item.file->output);
    const std::wstring sidecarTemporary = output + std::wstring(kTemporarySuffix);
    return write_whole(sidecarTemporary, item.sidecar) && publish(sidecarTemporary, output)
               ? Status::ok
               : Status::writeFailed;
}

} // namespace

/** A package id moves as itself; a tag keeps its entry and takes the new package's base. */
bool move_value(std::span<std::byte> bytes,
                Kind kind,
                std::span<const Move> moves,
                bool& changed) noexcept {
    changed = false;
    if (kind == Kind::package) {
        const auto id = internal::load<std::uint16_t>(bytes, 0);
        for (const Move& move : moves) {
            if (move.from == id) {
                internal::store(bytes, 0, move.to);
                changed = true;
            }
        }
        return true;
    }
    const auto tag = internal::load<std::uint32_t>(bytes, 0);
    if (tag < layout::kTagBase
        || layout::package_of(tag) > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    const auto package = static_cast<std::uint16_t>(layout::package_of(tag));
    for (const Move& move : moves) {
        if (move.from == package) {
            internal::store(bytes, 0, moved_tag(tag, move.from, move.to));
            changed = true;
        }
    }
    return true;
}

namespace {

/** Edits are planned on the sources, applied to copies, then the digests follow the bytes. */
[[nodiscard]] Status rewrite_copies(std::span<const File> files,
                                    std::span<const Location> locations,
                                    std::span<const Move> moves) noexcept {
    try {
        std::vector<Item> items(files.size());
        for (std::size_t index = 0; index < files.size(); ++index) {
            Item& item = items[index];
            item.file = &files[index];
            if (const Status status =
                    internal::read_package(files[index].source.c_str(), item.package);
                status != Status::ok) {
                return status;
            }
            if (!read_whole(sidecar_path(files[index].source), item.sidecar, item.hasSidecar)) {
                return Status::unreadable;
            }
        }
        std::vector<Handle> sources;
        for (const File& file : files) {
            sources.push_back(open_file(file.source, GENERIC_READ, OPEN_EXISTING));
            if (sources.back() == nullptr) {
                return Status::unreadable;
            }
        }
        std::set<std::tuple<std::uint16_t, std::uint16_t, std::uint32_t>> changedBlocks;
        for (const Location& location : locations) {
            std::vector<std::size_t> family;
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (items[index].package.packageId == location.package) {
                    family.push_back(index);
                }
            }
            if (family.empty()) {
                continue;
            }
            std::sort(family.begin(), family.end(), [&](std::size_t first, std::size_t second) {
                return items[first].package.patchId < items[second].package.patchId;
            });
            const std::size_t width =
                location.kind == Kind::package ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
            bool changed = false;
            if (location.entry == kMiscEntry || location.entry == kSidecarEntry) {
                for (const std::size_t index : family) {
                    Item& item = items[index];
                    std::vector<std::byte>& region =
                        location.entry == kMiscEntry ? item.package.misc : item.sidecar;
                    if (location.entry == kSidecarEntry && !item.hasSidecar) {
                        continue;
                    }
                    if (location.offset > region.size() || region.size() - location.offset < width
                        || !move_value(std::span(region).subspan(location.offset, width),
                                       location.kind == Kind::package ? Kind::package : Kind::tag,
                                       moves,
                                       changed)) {
                        return Status::format;
                    }
                    item.miscChanged =
                        item.miscChanged || (changed && location.entry == kMiscEntry);
                }
                continue;
            }
            std::size_t holder = 0;
            std::uint32_t block = 0;
            std::uint64_t at = 0;
            std::array<std::byte, 4> value{};
            if (!locate(items, family, location, width, holder, block, at)
                || !internal::read_exact(sources[holder].get(), at, std::span(value).first(width))
                || !move_value(std::span(value).first(width), location.kind, moves, changed)) {
                return Status::format;
            }
            if (changed) {
                items[holder].edits.emplace_back(at, value);
                items[holder].widths.push_back(width);
                changedBlocks.emplace(location.package, items[holder].package.patchId, block);
            }
        }
        if (const Status status = copy_and_edit(items); status != Status::ok) {
            return status;
        }
        for (const auto& [package, patch, block] : changedBlocks) {
            const auto holder = std::find_if(items.begin(), items.end(), [&](const Item& item) {
                return item.package.packageId == package && item.package.patchId == patch;
            });
            const layout::BlockRecord record = block_record(holder->package, block);
            std::vector<std::byte> bytes(record.size);
            const Handle copy = open_file(
                holder->file->output + std::wstring(kTemporarySuffix), GENERIC_READ, OPEN_EXISTING);
            sha1::Digest digest{};
            if (copy == nullptr || !internal::read_exact(copy.get(), record.offset, bytes)
                || !sha1::hash(bytes, digest)) {
                return Status::writeFailed;
            }
            for (Item& item : items) {
                if (item.package.packageId == package && block < item.package.blockCount
                    && block_record(item.package, block).patchId == patch) {
                    std::memcpy(item.package.metadata.data() + item.package.blockTable
                                    + std::size_t{block} * layout::kBlockRecordSize
                                    + BlockRecordField::kDigest,
                                digest.data(),
                                digest.size());
                    item.metadataChanged = true;
                }
            }
        }
        for (Item& item : items) {
            if (const Status status = finish(item, moves); status != Status::ok) {
                return status;
            }
        }
        return Status::ok;
    } catch (...) {
        return Status::capacity;
    }
}

} // namespace

/** A failed rewrite leaves no temporary copy behind; published outputs stay. */
Status rewrite(std::span<const File> files,
               std::span<const Location> locations,
               std::span<const Move> moves) noexcept {
    const Status status = rewrite_copies(files, locations, moves);
    if (status != Status::ok) {
        try {
            for (const File& file : files) {
                remove_file(file.output + std::wstring(kTemporarySuffix));
                remove_file(sidecar_path(file.output) + std::wstring(kTemporarySuffix));
            }
        } catch (...) {
            return status;
        }
    }
    return status;
}

} // namespace sunrise::middleware::content::packages::merge::remap
