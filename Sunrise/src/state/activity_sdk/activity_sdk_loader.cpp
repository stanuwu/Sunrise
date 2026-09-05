#include <Windows.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../middleware/crypto/sha256.h"
#include "internal.h"

namespace sunrise::state::activity_sdk {
namespace {

/** @param stage SDK load boundary that must be visible even if the next call does not return. */
void begin_load_stage(const char* stage) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::debug,
                      "ev=activity_sdk_load stage=%s phase=begin",
                      stage);
}

/**
 * Names a refused pack check without logging its contents.
 * @param stage Failed loader boundary.
 * @param reason Stable check name or validation refusal.
 * @return False for the caller's existing failure path.
 */
[[nodiscard]] bool refuse_load(const char* stage, const char* reason) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=activity_sdk_load stage=%s result=fail reason=%s",
                      stage,
                      reason);
    return false;
}

/**
 * Reports an error captured immediately after a failed Win32 call, before logging or cleanup.
 * @param stage Failed loader boundary.
 * @param error The failing call's GetLastError value.
 * @return False for the caller's existing failure path.
 */
[[nodiscard]] bool refuse_windows_load(const char* stage, DWORD error) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=activity_sdk_load stage=%s result=fail win32_error=%lu",
                      stage,
                      static_cast<unsigned long>(error));
    return false;
}

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
        return refuse_load("header", "layout_or_bounds");
    }
    if (header.sdkBuildSha256 != expected.sdkBuildSha256) {
        result = Status::wrongSdkBuild;
        return refuse_load("header", "sdk_build_mismatch");
    }
    if (header.payloadSha256 != expected.payloadSha256
        || header.contentKeySha256 != expected.contentKeySha256
        || header.logicalIrSha256 != expected.logicalIrSha256) {
        return refuse_load("header", "identity_mismatch");
    }
    middleware::crypto::sha256::Digest digest{};
    const auto payload = file.subspan(header.headerSize);
    begin_load_stage("payload_hash");
    if (!middleware::crypto::sha256::hash(payload, digest)) {
        return refuse_load("payload_hash", "hash_failed");
    }
    return digest == expected.payloadSha256 || refuse_load("payload_hash", "digest_mismatch");
}

/** Converts an exact Windows file size to process address space. */
[[nodiscard]] bool mapped_size(HANDLE file, std::size_t& output) noexcept {
    output = 0;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE) {
        return refuse_windows_load("file_size", GetLastError());
    }
    if (size.QuadPart < sizeof(format::Header)
        || static_cast<std::uint64_t>(size.QuadPart)
               > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return refuse_load("file_size", "invalid_size");
    }
    output = static_cast<std::size_t>(size.QuadPart);
    return true;
}

/** Resolves the exact parent directory before the pack mapping becomes public. */
[[nodiscard]] bool pack_directory(const wchar_t* path, std::wstring& output) noexcept {
    output.clear();
    core::path::Buffer absolute;
    wchar_t* filePart = nullptr;
    const DWORD copied = GetFullPathNameW(
        path, static_cast<DWORD>(absolute.chars.size()), absolute.chars.data(), &filePart);
    if (copied == 0) {
        return refuse_windows_load("pack_directory", GetLastError());
    }
    if (copied >= absolute.chars.size() || filePart == nullptr
        || filePart <= absolute.chars.data()) {
        return refuse_load("pack_directory", "invalid_path");
    }
    std::size_t length = static_cast<std::size_t>(filePart - absolute.chars.data());
    while (length != 0
           && (absolute.chars[length - 1] == L'\\' || absolute.chars[length - 1] == L'/')) {
        --length;
    }
    if (length == 0) {
        return refuse_load("pack_directory", "empty_directory");
    }
    try {
        output.assign(absolute.chars.data(), length);
        return true;
    } catch (...) {
        output.clear();
        return refuse_load("pack_directory", "allocation_failed");
    }
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
        return refuse_load("open", "invalid_path");
    }
    begin_load_stage("open");
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
        if (result == Status::missing) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             "ev=activity_sdk_load stage=open result=missing");
            return false;
        }
        return refuse_windows_load("open", error);
    }

    std::shared_ptr<Catalog> pending;
    try {
        pending = std::make_shared<Catalog>();
    } catch (...) {
        CloseHandle(file);
        return refuse_load("catalog_storage", "allocation_failed");
    }
    pending->file_ = file;
    begin_load_stage("pack_directory");
    if (!pack_directory(path, pending->artifactDirectory_)) {
        return false;
    }
    begin_load_stage("file_size");
    if (!mapped_size(file, pending->size_)) {
        return false;
    }
    begin_load_stage("file_mapping");
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        return refuse_windows_load("file_mapping", GetLastError());
    }
    pending->mapping_ = mapping;
    begin_load_stage("map_view");
    const void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        return refuse_windows_load("map_view", GetLastError());
    }
    pending->view_ = static_cast<const std::byte*>(view);
    pending->header_ = reinterpret_cast<const format::Header*>(pending->view_);
    const std::span<const std::byte> bytes(pending->view_, pending->size_);
    begin_load_stage("header");
    if (!identity::valid(expected.sdkBuildSha256) || !identity::valid(expected.payloadSha256)
        || !identity::valid(expected.contentKeySha256)
        || !identity::valid(expected.logicalIrSha256)) {
        return refuse_load("header", "expected_identity_unavailable");
    }
    if (!valid_header(*pending->header_, bytes, expected, result)) {
        return false;
    }
    begin_load_stage("catalog_validation");
    if (!valid_catalog(*pending)) {
        return refuse_load("catalog_validation", last_catalog_reason());
    }
    output = std::move(pending);
    result = Status::ready;
    core::log::write(core::log::Channel::state,
                     core::log::Level::info,
                     "ev=activity_sdk_load stage=load phase=complete result=ready");
    return true;
}

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Loads the compile-pinned regression fixture without exposing that trust path in production. */
bool load_path(const wchar_t* path, std::shared_ptr<Catalog>& output, Status& result) noexcept {
    const ExpectedIdentity expected{format::kExpectedSdkBuildSha256,
                                    format::kExpectedPayloadSha256,
                                    format::kExpectedContentKeySha256,
                                    format::kExpectedLogicalIrSha256};
    return load_path_expected(path, expected, output, result);
}

/** Applies one scoped synthetic payload pin. Production builds have no such entry point. */
bool load_path_for_test(const wchar_t* path,
                        const std::array<std::byte, 32>& expectedPayloadSha256,
                        std::shared_ptr<Catalog>& output,
                        Status& result) noexcept {
    const ExpectedIdentity expected{format::kExpectedSdkBuildSha256,
                                    expectedPayloadSha256,
                                    format::kExpectedContentKeySha256,
                                    format::kExpectedLogicalIrSha256};
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
