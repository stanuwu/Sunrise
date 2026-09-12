#include <Windows.h>

#include <array>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../middleware/crypto/sha256.h"
#include "internal.h"

namespace sunrise::state::activity_sdk {
namespace {

/** The SDK pack is installed beneath the module directory without creating it at read time. */
constexpr std::wstring_view kPackSuffix = L"Sunrise\\activity_sdk.pack";
/** Each section stride is fixed by the current runtime-pack ABI. */
constexpr std::array<std::uint32_t, format::kSectionCount> kExpectedStrides{
    1,
    sizeof(format::Activity),
    sizeof(format::Scenario),
    sizeof(format::Bubble),
    sizeof(format::State),
    sizeof(format::Object),
    sizeof(format::Occurrence),
    sizeof(format::Slot),
    sizeof(format::Text),
    sizeof(format::Capability),
    sizeof(format::Gate),
    sizeof(format::Refusal),
    sizeof(format::ActorClass),
    sizeof(format::RsatDescriptor),
    sizeof(format::RsatSchema),
    sizeof(format::RsatField),
    sizeof(format::Squad),
    sizeof(format::SquadMember),
    sizeof(format::SquadAnchor),
    sizeof(format::AuthoredSceneResource),
    sizeof(format::AuthoredSceneSquadEdge),
    sizeof(format::TaskTarget),
    sizeof(format::DialogueCueText),
    sizeof(format::DirectiveElement),
    sizeof(format::ActivityBindingTag),
    sizeof(format::ActivityBindingLocator),
    sizeof(format::BehaviorProgram),
    sizeof(format::BehaviorInput),
    sizeof(format::BehaviorChannelWrite),
    sizeof(format::BehaviorOwner),
    sizeof(format::BehaviorActivityBinding),
    sizeof(format::ActorMessageSchema),
    sizeof(format::ActorCommandDefinition),
    sizeof(format::ActorBehaviorProfile),
    sizeof(format::SimulationEventDefinition),
    sizeof(format::RuntimeSchema),
    sizeof(format::RuntimeField),
    sizeof(format::SobjectRsat),
    sizeof(format::SobjectRsatDescriptor),
    sizeof(format::EntityTypeDefinition),
    sizeof(format::SobjectRsatFieldBinding),
    sizeof(format::RuntimeTypeDefinition),
    sizeof(format::ActorStateName),
    sizeof(format::ActorSequenceTable),
    sizeof(format::ActorSequenceEntry),
    sizeof(format::ActorSequenceBinding),
};

/** Checks all section bounds before any typed row pointer is formed. */
[[nodiscard]] bool valid_sections(const format::Header& header) noexcept {
    std::uint64_t priorEnd = header.headerSize;
    for (std::size_t index = 0; index < header.sections.size(); ++index) {
        const format::Section& section = header.sections[index];
        const std::uint64_t stride = kExpectedStrides[index];
        if (section.stride != stride
            || section.count > (std::numeric_limits<std::uint64_t>::max)() / stride) {
            return false;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(section.count) * stride;
        if (section.offset != priorEnd || section.offset > header.fileSize
            || bytes > header.fileSize - section.offset) {
            return false;
        }
        priorEnd = section.offset + bytes;
    }
    return priorEnd == header.fileSize;
}

/** Checks every compiled provenance pin and hashes the exact mapped payload. */
[[nodiscard]] bool valid_header(const format::Header& header,
                                std::span<const std::byte> file,
                                const ExpectedIdentity& expected,
                                Status& result) noexcept {
    if (header.magic != format::kMagic || header.version != format::kVersion
        || header.headerSize != sizeof(format::Header) || header.fileSize != file.size()
        || header.sectionCount != format::kSectionCount || header.reserved != 0
        || !valid_sections(header)) {
        return false;
    }
    if (header.sdkBuildSha256 != expected.sdkBuildSha256) {
        result = Status::wrongSdkBuild;
        return false;
    }
    if (header.payloadSha256 != expected.payloadSha256
        || header.contentKeySha256 != expected.contentKeySha256
        || header.logicalIrSha256 != expected.logicalIrSha256) {
        return false;
    }
    middleware::crypto::sha256::Digest digest{};
    const auto payload = file.subspan(header.headerSize);
    return middleware::crypto::sha256::hash(payload, digest) && digest == expected.payloadSha256;
}

/** Converts an exact Windows file size to process address space. */
[[nodiscard]] bool mapped_size(HANDLE file, std::size_t& output) noexcept {
    output = 0;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < sizeof(format::Header)
        || static_cast<std::uint64_t>(size.QuadPart)
               > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    output = static_cast<std::size_t>(size.QuadPart);
    return true;
}

/** Resolves the exact parent directory before the pack mapping becomes public. */
[[nodiscard]] bool pack_directory(const wchar_t* path, core::path::Buffer& output) noexcept {
    output = {};
    core::path::Buffer absolute;
    wchar_t* filePart = nullptr;
    const DWORD copied = GetFullPathNameW(
        path, static_cast<DWORD>(absolute.chars.size()), absolute.chars.data(), &filePart);
    if (copied == 0 || copied >= absolute.chars.size() || filePart == nullptr
        || filePart <= absolute.chars.data()) {
        return false;
    }
    std::size_t length = static_cast<std::size_t>(filePart - absolute.chars.data());
    while (length != 0
           && (absolute.chars[length - 1] == L'\\' || absolute.chars[length - 1] == L'/')) {
        --length;
    }
    if (length == 0) {
        return false;
    }
    return core::path::assign(output, std::wstring_view(absolute.chars.data(), length));
}

} // namespace

/** Maps one explicit pack only after an independent expected identity is supplied. */
bool load_path_expected(const wchar_t* path,
                        const ExpectedIdentity& expected,
                        std::shared_ptr<Catalog>& output,
                        Status& result) noexcept {
    output.reset();
    result = Status::catalogInvalid;
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        result = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                     ? Status::missing
                     : Status::catalogInvalid;
        return false;
    }

    std::shared_ptr<Catalog> pending;
    try {
        pending = std::make_shared<Catalog>();
    } catch (...) {
        CloseHandle(file);
        return false;
    }
    pending->file_ = file;
    if (!pack_directory(path, pending->artifactDirectory_)) {
        return false;
    }
    if (!mapped_size(file, pending->size_)) {
        return false;
    }
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        return false;
    }
    pending->mapping_ = mapping;
    const void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        return false;
    }
    pending->view_ = static_cast<const std::byte*>(view);
    pending->header_ = reinterpret_cast<const format::Header*>(pending->view_);
    const std::span<const std::byte> bytes(pending->view_, pending->size_);
    if (!identity::valid(expected.sdkBuildSha256) || !identity::valid(expected.payloadSha256)
        || !identity::valid(expected.contentKeySha256) || !identity::valid(expected.logicalIrSha256)
        || !valid_header(*pending->header_, bytes, expected, result) || !valid_catalog(*pending)) {
        return false;
    }
    output = std::move(pending);
    result = Status::ready;
    return true;
}

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Loads the compile-pinned regression fixture without exposing that trust path in production. */
bool load_path(const wchar_t* path, std::shared_ptr<Catalog>& output, Status& result) noexcept {
    const ExpectedIdentity expected{format::kExpectedSdkBuildDigest,
                                    format::kExpectedPayloadDigest,
                                    format::kExpectedContentKeyDigest,
                                    format::kExpectedLogicalIrDigest};
    return load_path_expected(path, expected, output, result);
}

/** Applies one scoped synthetic payload pin. Production builds have no such entry point. */
bool load_path_for_test(const wchar_t* path,
                        const std::array<std::byte, 32>& expectedPayloadSha256,
                        std::shared_ptr<Catalog>& output,
                        Status& result) noexcept {
    const ExpectedIdentity expected{format::kExpectedSdkBuildDigest,
                                    expectedPayloadSha256,
                                    format::kExpectedContentKeyDigest,
                                    format::kExpectedLogicalIrDigest};
    return load_path_expected(path, expected, output, result);
}
#endif

/** Resolves the read-only installed pack path without creating its parent directory. */
bool load(void* module,
          const ExpectedIdentity& expected,
          std::shared_ptr<Catalog>& output,
          Status& result) noexcept {
    core::path::Buffer path;
    if (!core::path::module_directory(module, path) || !core::path::append(path, kPackSuffix)) {
        output.reset();
        result = Status::catalogInvalid;
        return false;
    }
    return load_path_expected(path.chars.data(), expected, output, result);
}

} // namespace sunrise::state::activity_sdk
