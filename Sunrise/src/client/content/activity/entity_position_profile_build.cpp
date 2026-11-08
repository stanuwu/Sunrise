#include "entity_position_profile_build.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/named_tags.h"
#include "../../../middleware/content/packages/reader/package_file.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808091.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808099.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809d.h"
#include "../../../middleware/content/packages/tables/entity_position_profile_extractor.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/content_manifest/content_manifest_state_runtime.h"
#include "../../../state/gameplay/external/entity_position_profiles.h"
#include "entity_object_type_build.h"

namespace sunrise::client::content::activity::entity_position_profiles {
namespace {
namespace profiles = state::gameplay::entity_position_profiles;
namespace extractor = middleware::content::packages::position_profiles;
namespace named = middleware::content::packages::named_tags;
namespace reader = middleware::content::packages::reader;
using Blob = std::vector<std::byte>;
namespace generated = middleware::content::packages::schema::generated;
/** One hash64 reference row of the package metadata. */
using ReferenceRow = generated::Hash64ReferenceRow;
/** One installed package directory never holds more `.pkg` files than this. */
constexpr std::size_t kMaximumPackageFileCount = 65536;
/** Package file header fields this pass reads before any metadata. */
using HeaderOffsets = reader::layout::HeaderOffsets;
using MiscRegion = reader::layout::MiscRegion;
/** The hash64 reference directory is refused above this size. */
constexpr std::uint64_t kMaximumMetadataBytes = 64ULL * 1024ULL * 1024ULL;
struct Name final {
    extractor::NamedTag value;
    std::uint32_t patch{};
    bool conflict{};
};
struct Names final {
    /** Sorted by name so the merge order matches the emitted row order. */
    std::vector<Name> rows;
    bool base{};
    std::uint32_t patch{};
};
struct Context final {
    const reader::Source& source;
    reader::Scratch& scratch;
};
/** One installed package file. The family is the leading part of the name before its patch. */
struct File final {
    std::array<wchar_t, MAX_PATH> name{};
    std::size_t familyLength{};
    std::uint32_t patch{};
};
/** One hash64 metadata reference and the package row it names. */
struct Reference final {
    std::uint64_t key{};
    std::uint32_t tag{};
    std::uint32_t classId{};
};
/** One merged reference. A key that two families disagree on is not emitted. */
struct MergedReference final {
    Reference row{};
    bool unique{true};
};
/** Closes one package handle on every exit path. */
class FileHandle final {
public:
    explicit FileHandle(const wchar_t* path) noexcept
        : handle_(CreateFileW(path,
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr)) {}

    ~FileHandle() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(handle_);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};
/** @return The family part of one package file name. */
[[nodiscard]] std::wstring_view family_of(const File& file) noexcept {
    return {file.name.data(), file.familyLength};
}
/** Joins one directory and child without doubling a separator an extended path would keep. */
[[nodiscard]] bool join_path(std::wstring_view directory,
                             std::wstring_view child,
                             core::path::Buffer& output) noexcept {
    if (!core::path::assign(output, directory)) {
        return false;
    }
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/'
        && !core::path::append(output, L"\\")) {
        return false;
    }
    return core::path::append(output, child);
}
/**
 * Reads one exact byte range. A short read is a failure.
 * @param file Open package handle.
 * @param offset Absolute byte offset.
 * @param output Receives the range; its size is the amount required.
 * @return True when the whole range was read.
 */
[[nodiscard]] bool
read_at(HANDLE file, std::uint64_t offset, std::span<std::byte> output) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(file, position, nullptr, FILE_BEGIN) == FALSE) {
        return false;
    }
    std::size_t done = 0;
    while (done < output.size()) {
        // One ReadFile call is bounded by its DWORD length.
        constexpr std::size_t kMaximumChunk = 0x10000000;
        const auto chunk = static_cast<DWORD>((std::min)(output.size() - done, kMaximumChunk));
        DWORD read = 0;
        if (ReadFile(file, output.data() + done, chunk, &read, nullptr) == FALSE || read == 0) {
            return false;
        }
        done += read;
    }
    return true;
}
/** Same-patch name conflicts cannot select an arbitrary package. */
bool collect_name(void* opaque, const named::Entry& entry) noexcept {
    try {
        auto& names = *static_cast<Names*>(opaque);
        if (entry.classId != generated::SMapRoot::kClass
            && entry.classId != generated::SScenario::kClass) {
            return true;
        }
        const std::string_view name(entry.name.data(), entry.nameLength);
        const auto found = std::lower_bound(
            names.rows.begin(), names.rows.end(), name, [](const Name& row, std::string_view key) {
                return row.value.text() < key;
            });
        const Name replacement{{name, entry.tag, entry.classId, names.base}, names.patch, false};
        if (found == names.rows.end() || found->value.text() != name) {
            names.rows.insert(found, replacement);
        } else if (found->patch < names.patch) {
            *found = replacement;
        } else if (found->patch == names.patch
                   && (found->value.tag != entry.tag || found->value.classId != entry.classId)) {
            found->conflict = true;
        }
        return true;
    } catch (...) {
        return false;
    }
}
template <class T> T value(const Blob& bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw 0;
    }
    T result{};
    std::memcpy(&result, bytes.data() + offset, sizeof result);
    return result;
}
/** Replaces one family reference, keeping the bank sorted by key. */
void set_reference(std::vector<Reference>& rows, const Reference& row) {
    const auto found = std::lower_bound(
        rows.begin(), rows.end(), row.key, [](const Reference& left, std::uint64_t key) {
            return left.key < key;
        });
    if (found != rows.end() && found->key == row.key) {
        *found = row;
        return;
    }
    rows.insert(found, row);
}
/** Folds one family's references in, marking any key two families disagree on. */
void merge_references(const std::vector<Reference>& family, std::vector<MergedReference>& merged) {
    for (const Reference& row : family) {
        const auto found = std::lower_bound(
            merged.begin(),
            merged.end(),
            row.key,
            [](const MergedReference& left, std::uint64_t key) { return left.row.key < key; });
        if (found == merged.end() || found->row.key != row.key) {
            merged.insert(found, MergedReference{row, true});
        } else if (found->row.tag != row.tag || found->row.classId != row.classId) {
            found->unique = false;
        }
    }
}
/** Parses one `<family>_<patch>.pkg` file name. @return False when the name has no patch suffix. */
[[nodiscard]] bool parse_file_name(std::wstring_view name, File& output) noexcept {
    output = {};
    if (name.size() <= reader::layout::kPackageFileExtension.size()
        || name.size() >= output.name.size()
        || name.substr(name.size() - reader::layout::kPackageFileExtension.size())
               != reader::layout::kPackageFileExtension) {
        return false;
    }
    const std::wstring_view stem =
        name.substr(0, name.size() - reader::layout::kPackageFileExtension.size());
    const std::size_t separator = stem.rfind(L'_');
    if (separator == std::wstring_view::npos || separator + 1 == stem.size()) {
        return false;
    }
    std::uint64_t patch = 0;
    for (const wchar_t character : stem.substr(separator + 1)) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        patch = patch * 10U + static_cast<std::uint64_t>(character - L'0');
        if (patch > UINT32_MAX) {
            return false;
        }
    }
    std::copy(name.begin(), name.end(), output.name.begin());
    output.familyLength = separator;
    output.patch = static_cast<std::uint32_t>(patch);
    return true;
}
/** Collects every installed package file in family and patch order. */
[[nodiscard]] bool collect_files(std::wstring_view directory, std::vector<File>& files) {
    core::path::Buffer search{};
    if (!join_path(directory, L"*", search)) {
        return false;
    }
    WIN32_FIND_DATAW entry{};
    const HANDLE find = FindFirstFileW(search.chars.data(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool complete = true;
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        File file{};
        const std::wstring_view name(entry.cFileName);
        if (name.size() <= reader::layout::kPackageFileExtension.size()
            || name.substr(name.size() - reader::layout::kPackageFileExtension.size())
                   != reader::layout::kPackageFileExtension) {
            continue;
        }
        if (files.size() >= kMaximumPackageFileCount || !parse_file_name(name, file)) {
            complete = false;
            break;
        }
        files.push_back(file);
    } while (FindNextFileW(find, &entry) != FALSE);
    (void)FindClose(find);
    if (!complete || files.empty()) {
        return false;
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
        return family_of(a) < family_of(b) || (family_of(a) == family_of(b) && a.patch < b.patch);
    });
    return true;
}
/** Metadata references merge patches within a family before cross-family conflict checks. */
bool inventory(std::wstring_view directory,
               std::vector<extractor::NamedTag>& names,
               std::vector<extractor::KeyTag>& keys) {
    std::vector<File> files;
    if (!collect_files(directory, files)) {
        return false;
    }
    Names collected;
    std::vector<Reference> family;
    std::vector<MergedReference> merged;
    std::wstring_view currentFamily{};
    core::path::Buffer path{};
    for (const auto& file : files) {
        if (family_of(file) != currentFamily) {
            merge_references(family, merged);
            family.clear();
            currentFamily = family_of(file);
        }
        collected.base = currentFamily.find(L"_activities_") == std::wstring_view::npos;
        collected.patch = file.patch;
        named::Result result{};
        if (!join_path(directory, std::wstring_view(file.name.data()), path)
            || !named::extract_file(path.chars.data(), &collect_name, &collected, result)) {
            return false;
        }
        const FileHandle handle(path.chars.data());
        LARGE_INTEGER length{};
        if (!handle.valid() || GetFileSizeEx(handle.get(), &length) == FALSE
            || length.QuadPart <= 0) {
            return false;
        }
        Blob header(reader::layout::kHeaderSize);
        if (!read_at(handle.get(), 0, header)
            || value<std::uint16_t>(header, HeaderOffsets::kVersion)
                   != reader::layout::kSupportedVersion) {
            return false;
        }
        // Beta metadata has no hash64 reference array.
        if (value<std::uint8_t>(header, HeaderOffsets::kVariant) == 0) {
            continue;
        }
        if (value<std::uint8_t>(header, HeaderOffsets::kVariant) != 1) {
            return false;
        }
        const auto offset = value<std::uint32_t>(header, HeaderOffsets::kMiscOffset),
                   size = value<std::uint32_t>(header, HeaderOffsets::kMiscSize);
        if (size == 0) {
            continue;
        }
        if (size > kMaximumMetadataBytes
            || static_cast<std::uint64_t>(offset) + size
                   > static_cast<std::uint64_t>(length.QuadPart)) {
            return false;
        }
        Blob metadata(size);
        if (!read_at(handle.get(), offset, metadata)) {
            return false;
        }
        if (metadata.size() < MiscRegion::kMinimumSize) {
            continue;
        }
        std::vector<std::size_t> offsets;
        if (!extractor::array(metadata,
                              MiscRegion::kReferenceArray,
                              sizeof(ReferenceRow),
                              ReferenceRow::kClass,
                              offsets)) {
            return false;
        }
        for (auto member : offsets) {
            set_reference(
                family,
                {value<std::uint64_t>(metadata, member + offsetof(ReferenceRow, wideHash)),
                 value<std::uint32_t>(metadata, member + offsetof(ReferenceRow, tag)),
                 value<std::uint32_t>(metadata, member + offsetof(ReferenceRow, classReference))});
        }
    }
    merge_references(family, merged);
    for (const MergedReference& row : merged) {
        if (row.unique) {
            keys.push_back({row.row.key, row.row.tag, row.row.classId});
        }
    }
    for (const Name& row : collected.rows) {
        if (!row.conflict) {
            names.push_back(row.value);
        }
    }
    return true;
}
/** Class checks apply to every live tag reached by the extraction. */
bool read(void* opaque, std::uint32_t tag, std::uint32_t expected, Blob& bytes) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    std::uint32_t actual{};
    return reader::read_tag(context.source, context.scratch, tag, bytes, actual)
           && actual == expected;
}
} // namespace
bool ready() noexcept {
    profiles::Fingerprint identity{};
    if (!state::content_manifest::build_fingerprint(identity)) {
        return false;
    }
    const bool positions = profiles::confirm(identity);
    const bool objects = state::gameplay::entity_object_types::confirm(identity);
    return positions && objects;
}
/** The package pass confirms shared-cache rows or publishes a complete extraction. */
bool build(const reader::Source& source, reader::Scratch& scratch) noexcept {
    try {
        profiles::Fingerprint identity{};
        // The catalog identity includes the installed package builds.
        if (!state::content_manifest::build_fingerprint(identity)) {
            return false;
        }
        const bool positions = profiles::confirm(identity);
        const bool objects = state::gameplay::entity_object_types::confirm(identity);
        if (positions && objects) {
            return true;
        }
        if (!objects && !entity_object_types::build(source, scratch, identity)) {
            return false;
        }
        if (positions) {
            state::build_data::invalidate_cache();
            return true;
        }
        profiles::reset();
        std::vector<extractor::NamedTag> names;
        std::vector<extractor::KeyTag> keys;
        profiles::Rows rows;
        Context context{source, scratch};
        if (!inventory(source.directory, names, keys)
            || !extractor::extract(names, keys, &read, &context, rows)) {
            return false;
        }
        const auto count = rows.size();
        const bool published = profiles::publish(std::move(rows), identity);
        if (published) {
            state::build_data::invalidate_cache();
        }
        char line[160]{};
        (void)std::snprintf(
            line, sizeof line, "entity_position_profiles source=packages rows=%zu", count);
        core::log::write(core::log::Channel::client, core::log::Level::info, line);
        return published;
    } catch (...) {
        return false;
    }
}
} // namespace sunrise::client::content::activity::entity_position_profiles
