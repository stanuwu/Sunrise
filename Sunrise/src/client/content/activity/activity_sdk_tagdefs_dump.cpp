#include "activity_sdk_tagdefs_dump.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../items/packages/internal.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal {
namespace {

/** Version of the raw dump layout; the Studio registry generator checks it. */
constexpr std::string_view kFormat = "sunrise-tagdefs-raw/2";
/** Self-relative i64 offsets in a record: the pointer map and the default-value block. */
constexpr std::uint64_t kPointerMapAt = offsetof(generated::PackageReflectionMetadata, pointerMap);
constexpr std::uint64_t kDefaultsAt = offsetof(generated::PackageReflectionMetadata, defaultValues);
/** Bounds that refuse a corrupt record instead of reading the whole image. */
constexpr std::uint64_t kMaximumMapWords = 65536;
constexpr std::uint64_t kMaximumDefaultBytes = std::uint64_t{1} << 20;
/** A default block starts with its u64 length, which counts the length itself. */
constexpr std::uint64_t kDefaultsHeader = 8;
/** The primitive size table has one u32 per field type code. */
constexpr std::uint32_t kPrimitiveCount = kTypeLimit;
/** A file below the Sunrise exports directory; only its directory is used. */
constexpr std::wstring_view kExportFile = L"exports\\tagdefs-raw.json";
/** Digest bytes kept in the file name, as two hex digits each. */
constexpr std::size_t kNameDigestBytes = 4;

void append_hex(std::string& text, std::uint64_t value, int digits) {
    char buffer[24]{};
    const int length = std::snprintf(
        buffer, sizeof buffer, "\"0x%0*llX\"", digits, static_cast<unsigned long long>(value));
    text.append(buffer, static_cast<std::size_t>(length));
}

/** Appends one byte as two lowercase hex digits. */
void append_hex_byte(std::string& text, unsigned value) {
    char digit[3]{};
    (void)std::snprintf(digit, sizeof digit, "%02x", value & 0xFFU);
    text.append(digit, 2);
}

void append_u32(std::string& text, std::uint32_t value) {
    text.append(std::to_string(value));
}

void append_i32(std::string& text, std::int32_t value) {
    text.append(std::to_string(value));
}

/** Non-finite floats have no JSON number form, so they are written as their bit pattern. */
void append_float(std::string& text, float value) {
    if (!std::isfinite(value)) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        append_hex(text, bits, 8);
        return;
    }
    char buffer[32]{};
    const int length = std::snprintf(buffer, sizeof buffer, "%.9g", static_cast<double>(value));
    text.append(buffer, static_cast<std::size_t>(length));
}

void append_property(std::string& text, const Property& property) {
    text.append("{\"nameHash\":");
    append_hex(text, property.nameHash, 8);
    text.append(",\"memberType\":");
    append_hex(text, property.memberType, 8);
    text.append(",\"byteOffset\":");
    append_u32(text, property.byteOffset);
    text.push_back('}');
}

/** Writes all twelve descriptor members, so the generator never needs the executable. */
void append_field(std::string& text, const Field& field) {
    text.append("{\"offset\":");
    append_u32(text, field.offset);
    text.append(",\"alternate\":");
    append_u32(text, field.alternate);
    text.append(",\"bit\":");
    append_u32(text, field.bit);
    text.append(",\"half\":");
    append_float(text, field.half);
    text.append(",\"type\":");
    append_u32(text, field.type);
    text.append(",\"presence\":");
    append_u32(text, field.presence);
    text.append(",\"parameter2\":");
    append_u32(text, field.parameter2);
    text.append(",\"nested\":");
    append_hex(text, field.nested, 8);
    text.append(",\"bias\":");
    append_i32(text, field.bias);
    text.append(",\"width\":");
    append_i32(text, field.width);
    text.append(",\"parameter3\":");
    append_hex(text, field.parameter3, 8);
    text.append(",\"parameter4\":");
    append_hex(text, field.parameter4, 8);
    text.push_back('}');
}

/**
 * Appends one parsed record as an open JSON object; the caller closes it.
 * @param address Record RVA for an executable record; zero for a package record.
 */
void append_record(std::string& text,
                   std::uint32_t handle,
                   std::uint64_t address,
                   const Metadata& record) {
    text.append("{\"handle\":");
    append_hex(text, handle, 8);
    text.append(record.package ? ",\"source\":\"package\"" : ",\"source\":\"exe\"");
    if (!record.package) {
        text.append(",\"rva\":");
        append_hex(text, address, 8);
    }
    text.append(",\"size\":");
    append_u32(text, record.size);
    text.append(",\"alignment\":");
    append_u32(text, record.alignment);
    text.append(",\"kind\":");
    append_u32(text, record.kind);
    text.append(",\"generic\":");
    if (record.hasGeneric) {
        append_hex(text, record.generic, 8);
    } else {
        text.append("null");
    }
    text.append(",\"codec\":");
    if (record.codec.present) {
        const Codec& codec = record.codec;
        text.append("{\"original\":");
        append_u32(text, codec.original);
        text.append(",\"serialized\":");
        append_u32(text, codec.serialized);
        text.append(",\"flags\":");
        append_hex(text, codec.flags, 8);
        text.append(",\"array\":");
        append_u32(text, codec.array);
        text.append(",\"fields\":[");
        for (std::size_t index = 0; index < codec.fields.size(); ++index) {
            if (index != 0) {
                text.push_back(',');
            }
            append_field(text, codec.fields[index]);
        }
        text.append("]}");
    } else {
        text.append("null");
    }
    text.append(",\"base\":");
    if (record.hasProperties) {
        append_property(text, record.base);
    } else {
        text.append("null");
    }
    text.append(",\"properties\":[");
    for (std::size_t index = 0; index < record.properties.size(); ++index) {
        if (index != 0) {
            text.push_back(',');
        }
        append_property(text, record.properties[index]);
    }
    text.push_back(']');
}

/** A package blob read through the Image accessors; an address is a blob offset. */
struct Blob final {
    std::span<const std::byte> bytes;

    [[nodiscard]] bool u8(std::uint64_t at, std::uint8_t& value) const noexcept {
        return read_at(bytes, static_cast<std::size_t>(at), value);
    }
    [[nodiscard]] bool u32(std::uint64_t at, std::uint32_t& value) const noexcept {
        return read_at(bytes, static_cast<std::size_t>(at), value);
    }
    [[nodiscard]] bool i64(std::uint64_t at, std::int64_t& value) const noexcept {
        return read_at(bytes, static_cast<std::size_t>(at), value);
    }
};

/**
 * Reads a record's pointer map and default block as raw words and bytes.
 * @tparam Bytes Image for an executable record, Blob for a package record.
 * @param address Record start in `source`.
 * @param blocks Receives `,"pointerMap":...,"defaults":...` for the record object.
 * @return False when an offset or length leaves the record source or passes a bound.
 */
template <typename Bytes>
[[nodiscard]] bool
append_blocks(const Bytes& source, std::uint64_t address, std::string& blocks, const char*& error) {
    blocks.append(",\"pointerMap\":");
    std::int64_t relative = 0;
    if (!source.i64(address + kPointerMapAt, relative)) {
        error = "pointer_map_read";
        return false;
    }
    if (relative == 0) {
        blocks.append("null");
    } else {
        const std::uint64_t map = address + kPointerMapAt + static_cast<std::uint64_t>(relative);
        std::int64_t count = 0;
        if (!source.i64(map, count) || count < 0
            || static_cast<std::uint64_t>(count) > kMaximumMapWords) {
            error = "pointer_map_count";
            return false;
        }
        blocks.push_back('[');
        for (std::int64_t index = 0; index < count; ++index) {
            std::uint32_t low = 0;
            std::uint32_t high = 0;
            const std::uint64_t word = map + 8 + 8 * static_cast<std::uint64_t>(index);
            if (!source.u32(word, low) || !source.u32(word + 4, high)) {
                error = "pointer_map_read";
                return false;
            }
            blocks.append(index == 0 ? "[" : ",[");
            append_u32(blocks, low);
            blocks.push_back(',');
            append_u32(blocks, high);
            blocks.push_back(']');
        }
        blocks.push_back(']');
    }
    blocks.append(",\"defaults\":");
    if (!source.i64(address + kDefaultsAt, relative)) {
        error = "defaults_read";
        return false;
    }
    if (relative == 0) {
        blocks.append("null");
        return true;
    }
    const std::uint64_t block = address + kDefaultsAt + static_cast<std::uint64_t>(relative);
    std::int64_t length = 0;
    if (!source.i64(block, length) || length < static_cast<std::int64_t>(kDefaultsHeader)
        || static_cast<std::uint64_t>(length) > kMaximumDefaultBytes) {
        error = "defaults_length";
        return false;
    }
    blocks.push_back('"');
    for (std::int64_t index = 0; index < length; ++index) {
        std::uint8_t value = 0;
        if (!source.u8(block + static_cast<std::uint64_t>(index), value)) {
            error = "defaults_read";
            return false;
        }
        append_hex_byte(blocks, value);
    }
    blocks.push_back('"');
    return true;
}

/** Records one refused record; the dump keeps going so coverage is measurable. */
void append_error(
    std::string& text, bool& first, std::uint32_t handle, const char* source, const char* error) {
    text.append(first ? "\n" : ",\n");
    first = false;
    text.append("{\"handle\":");
    append_hex(text, handle, 8);
    text.append(",\"source\":\"");
    text.append(source);
    text.append("\",\"error\":\"");
    text.append(error == nullptr ? "unknown" : error);
    text.append("\"}");
}

bool collect_tag(void* context, std::uint32_t tag) noexcept {
    try {
        static_cast<std::vector<std::uint32_t>*>(context)->push_back(tag);
        return true;
    } catch (...) {
        return false;
    }
}

/** Writes the whole text to `path`, replacing any earlier file. */
[[nodiscard]] bool write_file(const std::wstring& path, const std::string& text) noexcept {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::size_t written = 0;
    bool complete = true;
    while (written < text.size()) {
        const DWORD chunk =
            static_cast<DWORD>((std::min)(text.size() - written, std::size_t{1} << 30));
        DWORD wrote = 0;
        if (WriteFile(file, text.data() + written, chunk, &wrote, nullptr) == FALSE || wrote == 0) {
            complete = false;
            break;
        }
        written += wrote;
    }
    CloseHandle(file);
    return complete;
}

} // namespace

/** Builds the whole document in memory, then writes the file in one pass. */
Status dump_tagdefs(const Image& image,
                    const reader::Source* source,
                    std::wstring_view outputDirectory,
                    DumpResult& output) noexcept {
    output = {};
    if (outputDirectory.empty()) {
        return Status::invalidInput;
    }
    Digest build{};
    if (!middleware::crypto::sha256::hash(image.file(), build)) {
        return Status::hashFailed;
    }
    try {
        std::string text;
        text.reserve(std::size_t{32} << 20);
        text.append("{\"format\":\"");
        text.append(kFormat);
        text.append("\",\"build\":\"");
        for (const std::byte value : build) {
            append_hex_byte(text, static_cast<unsigned>(value));
        }
        text.append("\",\"slotCount\":");
        append_u32(text, static_cast<std::uint32_t>(image.slots().size()));
        text.append(",\"primitiveSizes\":[");
        for (std::uint32_t code = 0; code < kPrimitiveCount; ++code) {
            std::uint32_t size = 0;
            if (!image.u32(image.primitive_sizes() + 4ULL * code, size)) {
                return Status::reflectionInvalid;
            }
            if (code != 0) {
                text.push_back(',');
            }
            append_u32(text, size);
        }
        text.append("],\n\"records\":[");

        std::string errors;
        bool firstError = true;
        bool firstRecord = true;
        output.slots = static_cast<std::uint32_t>(image.slots().size());
        for (const auto& [handle, address] : image.slots()) {
            Metadata record{};
            const char* error = nullptr;
            std::string blocks;
            if (!parse_native(image, address, record, error)
                || !append_blocks(image, address, blocks, error)) {
                append_error(errors, firstError, handle, "exe", error);
                ++output.nativeErrors;
                continue;
            }
            text.append(firstRecord ? "\n" : ",\n");
            firstRecord = false;
            append_record(text, handle, address - image.image_base(), record);
            text.append(blocks);
            text.push_back('}');
            ++output.native;
        }

        if (source != nullptr) {
            std::vector<std::uint32_t> tags;
            reader::ScanResult scan{};
            if (!reader::scan_class(*source, kPackageMetadataClass, &collect_tag, &tags, scan)) {
                return Status::packageRead;
            }
            std::sort(tags.begin(), tags.end());
            tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
            const std::unique_ptr<reader::Scratch> scratch(new (std::nothrow) reader::Scratch());
            if (scratch == nullptr) {
                return Status::allocation;
            }
            std::vector<std::byte> blob;
            for (const std::uint32_t tag : tags) {
                std::uint32_t classId = 0;
                Metadata record{};
                const char* error = nullptr;
                if (!reader::read_tag(*source, *scratch, tag, blob, classId)
                    || classId != kPackageMetadataClass) {
                    append_error(errors, firstError, tag, "package", "package_read");
                    ++output.packageErrors;
                    continue;
                }
                std::string blocks;
                if (!parse_package(tag, blob, record, error)
                    || !append_blocks(Blob{blob}, 0, blocks, error)) {
                    append_error(errors, firstError, tag, "package", error);
                    ++output.packageErrors;
                    continue;
                }
                text.append(firstRecord ? "\n" : ",\n");
                firstRecord = false;
                append_record(text, tag, 0, record);
                text.append(blocks);
                text.push_back('}');
                ++output.package;
            }
            reader::close_files(*scratch);
        }
        text.append("],\n\"errors\":[");
        text.append(errors);
        text.append("]}\n");

        std::wstring path(outputDirectory);
        if (path.back() != L'\\' && path.back() != L'/') {
            path.push_back(L'\\');
        }
        path.append(L"tagdefs-raw.");
        wchar_t nameDigit[3]{};
        for (std::size_t index = 0; index < kNameDigestBytes; ++index) {
            std::swprintf(nameDigit, 3, L"%02x", static_cast<unsigned>(build[index]));
            path.append(nameDigit, 2);
        }
        path.append(L".json");
        return write_file(path, text) ? Status::ready : Status::writeFailed;
    } catch (...) {
        return Status::allocation;
    }
}

namespace {

/** Owned inputs of one boot export, released by its thread. */
struct BootExport final {
    reader::BlockKeys keys{};
    bool hasKeys{};
    core::path::Buffer directory{};
};

/**
 * Opens the executable and writes one dump.
 * @param executablePath Unpacked dump to read offline, or null for this process.
 * @param keys Package keys for the package records, or null to dump the executable only.
 * @return The dump status.
 */
[[nodiscard]] Status run(const wchar_t* executablePath,
                         const wchar_t* outputDirectory,
                         const reader::BlockKeys* keys) noexcept {
    const DWORD attributes = GetFileAttributesW(outputDirectory);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Status::invalidInput;
    }
    const bool live = executablePath == nullptr || executablePath[0] == L'\0';
    core::path::Buffer executable{};
    const void* module = nullptr;
    if (live) {
        const DWORD copied = GetModuleFileNameW(
            nullptr, executable.chars.data(), static_cast<DWORD>(executable.chars.size()));
        if (copied == 0 || copied >= executable.chars.size()) {
            return Status::executableUnavailable;
        }
        executable.length = copied;
        module = GetModuleHandleW(nullptr);
    } else if (!core::path::assign(executable, executablePath)) {
        return Status::invalidInput;
    }
    const auto image = std::unique_ptr<Image>(new (std::nothrow) Image());
    if (image == nullptr) {
        return Status::allocation;
    }
    const Status opened = image->open(executable.chars.data(), module);
    if (opened != Status::ready) {
        return opened;
    }
    core::path::Buffer packageDirectory{};
    reader::Source source{};
    // Package records are read only from the running client's own install.
    const bool packages =
        live && keys != nullptr && items::packages::package_directory(packageDirectory);
    if (packages) {
        source.directory =
            std::wstring_view(packageDirectory.chars.data(), packageDirectory.length);
        source.keys = keys;
        source.installedOnly = true;
    }
    DumpResult result{};
    const Status status =
        dump_tagdefs(*image, packages ? &source : nullptr, outputDirectory, result);
    core::log::writef(core::log::Channel::client,
                      status == Status::ready ? core::log::Level::info : core::log::Level::error,
                      "ev=tagdefs_dump status=%s slots=%u native=%u native_errors=%u package=%u "
                      "package_errors=%u",
                      status_name(status),
                      result.slots,
                      result.native,
                      result.nativeErrors,
                      result.package,
                      result.packageErrors);
    return status;
}

/** Runs one boot export off the activation thread, then erases and frees its keys. */
DWORD WINAPI boot_export_thread(void* parameter) noexcept {
    auto* work = static_cast<BootExport*>(parameter);
    (void)run(nullptr, work->directory.chars.data(), work->hasKeys ? &work->keys : nullptr);
    SecureZeroMemory(&work->keys, sizeof work->keys);
    delete work;
    return 0;
}

} // namespace

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal

namespace sunrise::client::content::activity::sdk_generation::decode_plans {

/** Reads the package keys on this thread; without them the worker dumps the executable only. */
void export_tagdefs_if_requested() noexcept {
    if (!core::settings::get().client.exports.tagDefinitions) {
        return;
    }
    auto* work = new (std::nothrow) internal::BootExport();
    if (work == nullptr) {
        return;
    }
    // Resolving the file creates the exports directory; the dump names its own file.
    if (!core::path::artifact_file(internal::kExportFile, work->directory)) {
        delete work;
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=tagdefs_dump status=fail reason=path");
        return;
    }
    while (work->directory.length != 0
           && work->directory.chars[work->directory.length - 1] != L'\\') {
        --work->directory.length;
    }
    work->directory.chars[work->directory.length] = L'\0';
    // Keys are read here, on the activation thread, where the key export also reads them.
    work->hasKeys = items::packages::collect_keys(work->keys);
    const HANDLE thread = CreateThread(nullptr, 0, &internal::boot_export_thread, work, 0, nullptr);
    if (thread == nullptr) {
        SecureZeroMemory(&work->keys, sizeof work->keys);
        delete work;
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=tagdefs_dump status=fail reason=thread");
        return;
    }
    CloseHandle(thread);
}

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans

/**
 * Dumps the tag reflection records for the Studio registry generator.
 * @param executablePath Unpacked executable dump to read offline; null reads this process, its
 * module and its installed packages.
 * @param outputDirectory Existing directory that receives tagdefs-raw.<sha8>.json.
 * @return The decode_plans::Status value; 0 is ready.
 */
extern "C" __declspec(dllexport) std::uint32_t
SunriseDumpTagdefs(const wchar_t* executablePath, const wchar_t* outputDirectory) noexcept {
    namespace plans = sunrise::client::content::activity::sdk_generation::decode_plans;
    if (outputDirectory == nullptr || outputDirectory[0] == L'\0') {
        return static_cast<std::uint32_t>(plans::Status::invalidInput);
    }
    const bool live = executablePath == nullptr || executablePath[0] == L'\0';
    plans::internal::reader::BlockKeys keys{};
    const bool hasKeys = live && sunrise::client::content::items::packages::collect_keys(keys);
    const plans::Status status =
        plans::internal::run(executablePath, outputDirectory, hasKeys ? &keys : nullptr);
    SecureZeroMemory(&keys, sizeof keys);
    return static_cast<std::uint32_t>(status);
}