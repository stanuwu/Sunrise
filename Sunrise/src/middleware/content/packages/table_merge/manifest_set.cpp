#include <algorithm>
#include <optional>

#include "../reader/typed_array.h"
#include "../schema/generated/tag_layout_808001_0.h"
#include "../schema/generated/tag_layout_80809e_1.h"
#include "../tables/definition_index_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::table_merge::internal {
namespace {

using schema::generated::ManifestGroup;
using schema::generated::PackageManifest;

/** Manifest fields; an offset field is relative to its own position. */
struct ManifestField final {
    // Package manifest layout.
    static constexpr std::size_t kSize = offsetof(PackageManifest, tagSize);
    static constexpr std::size_t kOwnTag = offsetof(PackageManifest, selfTag);
    static constexpr std::size_t kRootTag = offsetof(PackageManifest, rootTag);
    static constexpr std::size_t kGroupCount =
        offsetof(PackageManifest, fragment.manifestArray.count);
    static constexpr std::size_t kGroupArray =
        offsetof(PackageManifest, fragment.manifestArray.relative);
    static constexpr std::size_t kKeyCount = offsetof(PackageManifest, fragment.keyCount.count);
    static constexpr std::size_t kKeyArray = offsetof(PackageManifest, fragment.keyCount.relative);
    /** The reader needs the key offset field whole. */
    static constexpr std::size_t kMinimumSize = sizeof(PackageManifest);
};

/** Fields of one group row, sorted by package id. */
struct GroupField final {
    // Manifest group row layout.
    static constexpr std::size_t kPackageId = offsetof(ManifestGroup, packageId);
    static constexpr std::size_t kBitmapCount =
        offsetof(ManifestGroup, bitArray.normalizeFlags.count);
    static constexpr std::size_t kBitmap =
        offsetof(ManifestGroup, bitArray.normalizeFlags.relative);
    static constexpr std::size_t kListCount = offsetof(ManifestGroup, indexArray.count);
    static constexpr std::size_t kList = offsetof(ManifestGroup, indexArray.relative);
    static constexpr std::size_t kSize = sizeof(ManifestGroup);
};

/** Group rows, bitmaps and entry lists are typed arrays. */
using ArrayField = reader::layout::TypedArrayHeader;

/** Element class of the group rows. */
constexpr std::uint32_t kGroupClass = ManifestGroup::kClass;
/** Element classes of the bitmap words and the u16 entry lists. */
constexpr std::uint32_t kBitmapClass = reader::layout::kU32ArrayClass;
constexpr std::size_t kBitmapWordSize = sizeof(schema::generated::U32ArrayRow);
constexpr std::uint32_t kEntryListClass = reader::layout::kU16ArrayClass;
/** One entry-list row is a u16 entry index. */
constexpr std::size_t kEntryListWordSize = sizeof(schema::generated::SU16Element);
static_assert(kEntryListWordSize == sizeof(std::uint16_t));
/** Entry indices keep 13 bits, the package entry limit. */
constexpr std::uint32_t kEntryMask = reader::layout::kTagEntryMask;
/** Bits in one bitmap word. */
constexpr std::uint32_t kWordBits = 32;
/** Bytes of one record key. */
constexpr std::size_t kKeySize = sizeof(schema::generated::C8080013d);
/** Manifest arrays start on 8-byte boundaries. */
constexpr std::size_t kArrayAlignment = 8;

/** One package's listed entries, sorted and unique. */
struct Group final {
    std::uint16_t packageId{};
    std::vector<std::uint16_t> entries;
};

/** @return The rows of the typed array whose descriptor is at `field`, when they fit and match. */
[[nodiscard]] std::optional<tables::Array> array_at(std::span<const std::byte> data,
                                                    std::size_t field,
                                                    std::uint32_t elementClass,
                                                    std::size_t stride) noexcept {
    tables::Array array{};
    if (!tables::find_optional_array_at(data, field, array)
        || (array.count != 0 && array.elementClass != elementClass)
        || array.dataOffset > data.size()
        || array.count > (data.size() - array.dataOffset) / stride) {
        return std::nullopt;
    }
    return array;
}

/** @return Where the bitmap words start, when the header matches and the words fit. */
[[nodiscard]] std::optional<std::size_t>
bitmap_words(std::span<const std::byte> data, std::size_t row, std::uint64_t words) noexcept {
    const std::size_t field = row + GroupField::kBitmap;
    const auto relative = load<std::int64_t>(data, field);
    if (relative < -static_cast<std::int64_t>(field)
        || relative > static_cast<std::int64_t>(data.size())) {
        return std::nullopt;
    }
    const auto header = static_cast<std::size_t>(static_cast<std::int64_t>(field) + relative);
    reader::TypedArrayRows rows{};
    if (!reader::read_typed_array(data, header, words, kBitmapClass, kBitmapWordSize, rows)) {
        return std::nullopt;
    }
    return rows.data;
}

/** @return True after adding every entry the group's bitmap marks. */
[[nodiscard]] bool
read_bitmap(std::span<const std::byte> data, std::size_t row, std::vector<std::uint16_t>& out) {
    const auto words = load<std::uint64_t>(data, row + GroupField::kBitmapCount);
    if (load<std::int64_t>(data, row + GroupField::kBitmap) == 0) {
        return words == 0;
    }
    const auto first = bitmap_words(data, row, words);
    if (!first) {
        return false;
    }
    for (std::uint64_t word = 0; word < words; ++word) {
        const auto bits =
            load<std::uint32_t>(data, *first + static_cast<std::size_t>(word) * kBitmapWordSize);
        for (std::uint32_t bit = 0; bit < kWordBits; ++bit) {
            const std::uint64_t entry = std::uint64_t{word} * kWordBits + bit;
            if ((bits & (1U << bit)) == 0) {
                continue;
            }
            // A bit past the entry limit names no entry a group can hold.
            if (entry > kEntryMask) {
                return false;
            }
            out.push_back(static_cast<std::uint16_t>(entry));
        }
    }
    return true;
}

/** @return True after adding every entry of the group's u16 list. */
[[nodiscard]] bool
read_list(std::span<const std::byte> data, std::size_t row, std::vector<std::uint16_t>& out) {
    const auto list =
        array_at(data, row + GroupField::kListCount, kEntryListClass, kEntryListWordSize);
    if (!list || list->count > std::uint64_t{kEntryMask} + 1) {
        return false;
    }
    for (std::uint64_t i = 0; i < list->count; ++i) {
        out.push_back(static_cast<std::uint16_t>(
            load<std::uint16_t>(data, list->dataOffset + i * kEntryListWordSize) & kEntryMask));
    }
    return true;
}

/**
 * Reads every group of one manifest. Throws std::bad_alloc.
 * @return False when an offset or a count leaves the data.
 */
[[nodiscard]] bool read_groups(std::span<const std::byte> data, std::vector<Group>& groups) {
    groups.clear();
    if (data.size() < ManifestField::kMinimumSize) {
        return false;
    }
    const auto array = array_at(data, ManifestField::kGroupCount, kGroupClass, GroupField::kSize);
    if (!array) {
        return false;
    }
    groups.resize(static_cast<std::size_t>(array->count));
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const std::size_t row = array->dataOffset + g * GroupField::kSize;
        Group& group = groups[g];
        group.packageId = load<std::uint16_t>(data, row + GroupField::kPackageId);
        if (!read_bitmap(data, row, group.entries) || !read_list(data, row, group.entries)) {
            return false;
        }
        std::sort(group.entries.begin(), group.entries.end());
        group.entries.erase(std::unique(group.entries.begin(), group.entries.end()),
                            group.entries.end());
    }
    return true;
}

/** Writes one typed u16 entry list whose header starts at `header`. */
void write_list(std::span<std::byte> data,
                std::size_t header,
                std::span<const std::uint16_t> entries) noexcept {
    store<std::uint32_t>(data, header - ArrayField::kMarkerBack, reader::layout::kTableArrayMarker);
    store<std::uint64_t>(data, header + ArrayField::kCount, entries.size());
    store<std::uint32_t>(data, header + ArrayField::kClass, kEntryListClass);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        store<std::uint16_t>(data, header + ArrayField::kSize + i * kEntryListWordSize, entries[i]);
    }
}

/** One relative offset field and the absolute position it names. */
struct Link final {
    std::size_t field{};
    std::size_t target{};
};

/**
 * Inserts a group row for a package the manifest lacks, in package order.
 * Every offset that crosses the new row moves; the entry list goes at the end.
 * @return ok, or unsupported when the manifest has no group array to grow.
 */
[[nodiscard]] Status add_group(std::vector<std::byte>& data,
                               std::span<const Group> groups,
                               std::uint16_t packageId,
                               std::span<const std::uint16_t> entries) {
    const auto array = array_at(data, ManifestField::kGroupCount, kGroupClass, GroupField::kSize);
    if (!array || array->count == 0 || entries.empty()) {
        return Status::unsupported;
    }
    const auto count = static_cast<std::int64_t>(array->count);
    const std::size_t header = array->dataOffset - ArrayField::kSize;
    const auto before = std::find_if(groups.begin(), groups.end(), [&](const Group& group) {
        return group.packageId >= packageId;
    });
    const std::size_t insert =
        array->dataOffset + static_cast<std::size_t>(before - groups.begin()) * GroupField::kSize;
    std::vector<Link> links{{ManifestField::kGroupArray, header}};
    if (load<std::int64_t>(data, ManifestField::kKeyCount) != 0
        && load<std::int64_t>(data, ManifestField::kKeyArray) != 0) {
        links.push_back(
            {ManifestField::kKeyArray,
             static_cast<std::size_t>(static_cast<std::int64_t>(ManifestField::kKeyArray)
                                      + load<std::int64_t>(data, ManifestField::kKeyArray))});
    }
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const std::size_t row = array->dataOffset + g * GroupField::kSize;
        for (const std::size_t field : {row + GroupField::kBitmap, row + GroupField::kList}) {
            const auto relative = load<std::int64_t>(data, field);
            if (relative != 0) {
                links.push_back(
                    {field, static_cast<std::size_t>(static_cast<std::int64_t>(field) + relative)});
            }
        }
    }
    const auto shift = [&](std::size_t offset) {
        return offset >= insert ? offset + GroupField::kSize : offset;
    };
    const auto listHeader = static_cast<std::size_t>(
        align(data.size() + GroupField::kSize, kArrayAlignment) + ArrayField::kSize);
    std::vector<std::byte> result(listHeader + ArrayField::kSize
                                  + entries.size() * kEntryListWordSize);
    std::copy_n(data.begin(), insert, result.begin());
    std::copy(data.begin() + static_cast<std::ptrdiff_t>(insert),
              data.end(),
              result.begin() + static_cast<std::ptrdiff_t>(insert + GroupField::kSize));
    for (const Link& link : links) {
        store<std::int64_t>(result,
                            shift(link.field),
                            static_cast<std::int64_t>(shift(link.target))
                                - static_cast<std::int64_t>(shift(link.field)));
    }
    store<std::uint16_t>(result, insert + GroupField::kPackageId, packageId);
    store<std::int64_t>(
        result, insert + GroupField::kListCount, static_cast<std::int64_t>(entries.size()));
    store<std::int64_t>(result,
                        insert + GroupField::kList,
                        static_cast<std::int64_t>(listHeader)
                            - static_cast<std::int64_t>(insert + GroupField::kList));
    write_list(result, listHeader, entries);
    store<std::int64_t>(result, ManifestField::kGroupCount, count + 1);
    store<std::int64_t>(result, header + ArrayField::kCount, count + 1);
    static_assert(ManifestField::kSize == 0);
    reader::layout::write_size_word(result);
    data = std::move(result);
    return Status::ok;
}

/**
 * Adds entries to one package's group; a new package gets a new group.
 * A grown group's bitmap and list become one typed list at the end.
 * @return ok, or why the manifest cannot grow.
 */
[[nodiscard]] Status add_entries(std::vector<std::byte>& data,
                                 std::uint16_t packageId,
                                 std::span<const std::uint16_t> entries) {
    std::vector<Group> groups;
    if (!read_groups(data, groups)) {
        return Status::format;
    }
    const auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& group) {
        return group.packageId == packageId;
    });
    if (found == groups.end()) {
        return add_group(data, groups, packageId, entries);
    }
    std::vector<std::uint16_t> combined = found->entries;
    combined.insert(combined.end(), entries.begin(), entries.end());
    std::sort(combined.begin(), combined.end());
    combined.erase(std::unique(combined.begin(), combined.end()), combined.end());
    if (combined.size() == found->entries.size()) {
        return Status::ok;
    }
    const auto array = array_at(data, ManifestField::kGroupCount, kGroupClass, GroupField::kSize);
    if (!array) {
        return Status::format;
    }
    const std::size_t row =
        array->dataOffset + static_cast<std::size_t>(found - groups.begin()) * GroupField::kSize;
    const auto header =
        static_cast<std::size_t>(align(data.size() + ArrayField::kMarkerBack, kArrayAlignment));
    data.resize(header + ArrayField::kSize + combined.size() * kEntryListWordSize);
    store<std::uint64_t>(data, row + GroupField::kBitmapCount, 0);
    store<std::int64_t>(data, row + GroupField::kBitmap, 0);
    store<std::int64_t>(
        data, row + GroupField::kListCount, static_cast<std::int64_t>(combined.size()));
    store<std::int64_t>(data,
                        row + GroupField::kList,
                        static_cast<std::int64_t>(header)
                            - static_cast<std::int64_t>(row + GroupField::kList));
    write_list(data, header, combined);
    store<std::uint64_t>(data, ManifestField::kSize, data.size());
    return Status::ok;
}

/** @return The key array bytes from its marker to its last key, or empty when it has none. */
[[nodiscard]] std::optional<std::span<const std::byte>>
key_bytes(std::span<const std::byte> data) noexcept {
    tables::Array keys{};
    if (!tables::find_optional_array_at(data, ManifestField::kKeyCount, keys)
        || keys.dataOffset > data.size()
        || keys.count > (data.size() - keys.dataOffset) / kKeySize) {
        return std::nullopt;
    }
    if (keys.count == 0) {
        return std::span<const std::byte>{};
    }
    const std::size_t marker = keys.dataOffset - ArrayField::kSize - ArrayField::kMarkerBack;
    return data.subspan(marker,
                        ArrayField::kMarkerBack + ArrayField::kSize + keys.count * kKeySize);
}

/** @return True when both manifests name the same tag, root and record keys. */
[[nodiscard]] bool same_identity(std::span<const std::byte> first,
                                 std::span<const std::byte> second) noexcept {
    const auto firstKeys = key_bytes(first);
    const auto secondKeys = key_bytes(second);
    return load<std::uint32_t>(first, ManifestField::kOwnTag)
               == load<std::uint32_t>(second, ManifestField::kOwnTag)
           && load<std::uint32_t>(first, ManifestField::kRootTag)
                  == load<std::uint32_t>(second, ManifestField::kRootTag)
           && load<std::int64_t>(first, ManifestField::kKeyCount)
                  == load<std::int64_t>(second, ManifestField::kKeyCount)
           && firstKeys && secondKeys
           && std::equal(
               firstKeys->begin(), firstKeys->end(), secondKeys->begin(), secondKeys->end());
}

} // namespace

/** Adds every group entry of `later` that the output lacks. */
Status union_manifest(std::span<const std::byte> later, std::vector<std::byte>& output) {
    if (later.size() < ManifestField::kMinimumSize || output.size() < ManifestField::kMinimumSize) {
        return Status::format;
    }
    if (!same_identity(output, later)) {
        return Status::mismatch;
    }
    std::vector<Group> groups;
    if (!read_groups(later, groups)) {
        return Status::format;
    }
    for (const Group& group : groups) {
        if (group.entries.empty()) {
            continue;
        }
        const Status status = add_entries(output, group.packageId, group.entries);
        if (status != Status::ok) {
            return status;
        }
    }
    return Status::ok;
}

} // namespace sunrise::middleware::content::packages::table_merge::internal
