#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/filesystem/temporary_sibling.h"
#include "../../../middleware/crypto/sha256.h"
#include "pack_writer.h"

namespace sunrise::state::activity_sdk::generation::pack {
namespace {

/** The section strides are part of the current runtime-pack ABI. */
constexpr std::array<std::uint32_t, format::kSectionCount> kSectionStrides{
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
/** Writer-owned siblings use the cleanup service's final.process.thread.sequence.tmp shape. */
constexpr std::wstring_view kTemporarySuffix = L".%08lX.%08lX.%08lX.tmp";
/** Bounded retries handle a stale sibling with the same process-local sequence. */
constexpr std::size_t kTemporaryAttempts = 16;

volatile LONG g_temporarySequence{};

/** One type-erased section keeps its row count separate from its byte view. */
struct RawSection final {
    std::span<const std::byte> bytes{};
    std::size_t count{};
    std::uint32_t stride{};
};

/** Small Windows file owner used by the all-or-nothing publication path. */
class FileHandle final {
public:
    /** Takes ownership of one exact Windows handle value. */
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    /** Releases the handle when the publication path exits early. */
    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    /** @return True when the handle was already empty or closed now. */
    [[nodiscard]] bool close() noexcept {
        if (value_ == INVALID_HANDLE_VALUE) {
            return true;
        }
        const HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        return CloseHandle(value) != FALSE;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

template <typename Value> [[nodiscard]] RawSection rows(std::span<const Value> values) noexcept {
    static_assert(std::is_trivially_copyable_v<Value> && std::is_standard_layout_v<Value>);
    return {std::as_bytes(values), values.size(), static_cast<std::uint32_t>(sizeof(Value))};
}

/**
 * Returns all borrowed sections in the one order fixed by `SectionIndex`.
 * @param tables Canonical string bytes and fixed-layout rows.
 * @return Type-erased spans with exact row counts and strides.
 */
[[nodiscard]] std::array<RawSection, format::kSectionCount>
raw_sections(const Tables& tables) noexcept {
    return {
        RawSection{tables.strings, tables.strings.size(), 1},
        rows(tables.activities),
        rows(tables.scenarios),
        rows(tables.bubbles),
        rows(tables.states),
        rows(tables.objects),
        rows(tables.occurrences),
        rows(tables.slots),
        rows(tables.texts),
        rows(tables.capabilities),
        rows(tables.gates),
        rows(tables.refusals),
        rows(tables.actorClasses),
        rows(tables.rsatDescriptors),
        rows(tables.rsatSchemas),
        rows(tables.rsatFields),
        rows(tables.squads),
        rows(tables.squadMembers),
        rows(tables.squadAnchors),
        rows(tables.authoredSceneResources),
        rows(tables.authoredSceneSquadEdges),
        rows(tables.taskTargets),
        rows(tables.dialogueCueTexts),
        rows(tables.directiveElements),
        rows(tables.activityBindingTags),
        rows(tables.activityBindingLocators),
        rows(tables.behaviorPrograms),
        rows(tables.behaviorInputs),
        rows(tables.behaviorChannelWrites),
        rows(tables.behaviorOwners),
        rows(tables.behaviorActivityBindings),
        rows(tables.actorMessageSchemas),
        rows(tables.actorCommandDefinitions),
        rows(tables.actorBehaviorProfiles),
        rows(tables.simulationEventDefinitions),
        rows(tables.runtimeSchemas),
        rows(tables.runtimeFields),
        rows(tables.sobjectRsats),
        rows(tables.sobjectRsatDescriptors),
        rows(tables.entityTypeDefinitions),
        rows(tables.sobjectRsatFieldBindings),
        rows(tables.runtimeTypeDefinitions),
        rows(tables.actorStateNames),
        rows(tables.actorSequenceTables),
        rows(tables.actorSequenceEntries),
        rows(tables.actorSequenceBindings),
    };
}

/**
 * Checks the fixed header and every contiguous section range before hashing payload bytes.
 * @param header Copied header that is safe to read at native alignment.
 * @param actualSize Complete caller-owned buffer size.
 * @return True when every range stays contiguous and exact.
 */
[[nodiscard]] bool valid_shape(const format::Header& header, std::size_t actualSize) noexcept {
    if (header.magic != format::kMagic || header.version != format::kVersion
        || header.headerSize != sizeof(format::Header) || header.fileSize != actualSize
        || header.sectionCount != format::kSectionCount || header.reserved != 0) {
        return false;
    }

    std::uint64_t priorEnd = header.headerSize;
    for (std::size_t index = 0; index < header.sections.size(); ++index) {
        const format::Section& section = header.sections[index];
        const std::uint64_t stride = kSectionStrides[index];
        if (section.stride != stride
            || section.count > (std::numeric_limits<std::uint64_t>::max)() / stride) {
            return false;
        }
        const std::uint64_t size = static_cast<std::uint64_t>(section.count) * stride;
        if (section.offset != priorEnd || section.offset > header.fileSize
            || size > header.fileSize - section.offset) {
            return false;
        }
        priorEnd = section.offset + size;
    }
    return priorEnd == header.fileSize;
}

/**
 * Fills section descriptors without narrowing overflow.
 * @param inputs Type-erased sections in canonical order.
 * @param header Receives exact offsets, counts, and strides.
 * @param fileSize Receives the complete file size only on success.
 * @return `ready` only when every range fits the format fields and process address space.
 */
[[nodiscard]] Status describe_sections(const std::array<RawSection, format::kSectionCount>& inputs,
                                       format::Header& header,
                                       std::size_t& fileSize) noexcept {
    std::uint64_t cursor = sizeof(format::Header);
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const RawSection& input = inputs[index];
        if (input.count > (std::numeric_limits<std::uint32_t>::max)()
            || input.stride != kSectionStrides[index]
            || input.count > (std::numeric_limits<std::uint64_t>::max)() / input.stride) {
            return Status::sizeOverflow;
        }
        const std::uint64_t size = static_cast<std::uint64_t>(input.count) * input.stride;
        if (input.bytes.size() != size
            || cursor > (std::numeric_limits<std::uint64_t>::max)() - size) {
            return Status::invalidInput;
        }
        header.sections[index] = {cursor, static_cast<std::uint32_t>(input.count), input.stride};
        cursor += size;
    }
    if (cursor > (std::numeric_limits<std::size_t>::max)()) {
        return Status::sizeOverflow;
    }
    fileSize = static_cast<std::size_t>(cursor);
    return Status::ready;
}

/**
 * Copies each complete section to the absolute range committed in the header.
 * @param inputs Borrowed source sections in canonical order.
 * @param header Complete checked section descriptors.
 * @param output Zeroed file image with checked capacity.
 */
void copy_sections(const std::array<RawSection, format::kSectionCount>& inputs,
                   const format::Header& header,
                   std::span<std::byte> output) noexcept {
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const RawSection& input = inputs[index];
        if (input.bytes.empty()) {
            continue;
        }
        const std::size_t offset = static_cast<std::size_t>(header.sections[index].offset);
        std::memcpy(output.data() + offset, input.bytes.data(), input.bytes.size());
    }
}

/**
 * Appends one fixed writer identity suffix to a final path.
 * @param finalPath Null-terminated final pack path.
 * @param output Receives the complete sibling path on success.
 * @return True when the complete path fits fixed storage.
 */
[[nodiscard]] bool make_temporary_path(const wchar_t* finalPath,
                                       core::path::Buffer& output) noexcept {
    /** The fixed suffix is shorter than this local buffer on every Windows target. */
    constexpr std::size_t kSuffixCapacity = 64;
    std::array<wchar_t, kSuffixCapacity> suffix{};
    const DWORD sequence = static_cast<DWORD>(InterlockedIncrement(&g_temporarySequence));
    const int length = std::swprintf(suffix.data(),
                                     suffix.size(),
                                     kTemporarySuffix.data(),
                                     GetCurrentProcessId(),
                                     GetCurrentThreadId(),
                                     sequence);
    return length > 0 && static_cast<std::size_t>(length) < suffix.size()
           && core::path::assign(output, finalPath)
           && core::path::append(output, std::wstring_view(suffix.data(), length));
}

/**
 * Writes all bytes and refuses a short successful Windows write.
 * @param file Open writer-owned file.
 * @param bytes Complete file image.
 * @return True when every byte reached the file handle.
 */
[[nodiscard]] bool write_all(HANDLE file, std::span<const std::byte> bytes) noexcept {
    while (!bytes.empty()) {
        const std::size_t count =
            (std::min)(bytes.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
        DWORD transferred = 0;
        if (WriteFile(file, bytes.data(), static_cast<DWORD>(count), &transferred, nullptr) == FALSE
            || transferred != count) {
            return false;
        }
        bytes = bytes.subspan(count);
    }
    return true;
}

/**
 * Creates and closes one complete writer-owned sibling.
 * @param finalPath Null-terminated final pack path.
 * @param bytes Complete deterministic file image.
 * @param temporaryPath Receives the created sibling path on success.
 * @return True when one complete sibling closed cleanly.
 */
[[nodiscard]] bool write_temporary(const wchar_t* finalPath,
                                   std::span<const std::byte> bytes,
                                   core::path::Buffer& temporaryPath) noexcept {
    core::path::remove_stale_siblings(finalPath);
    for (std::size_t attempt = 0; attempt < kTemporaryAttempts; ++attempt) {
        if (!make_temporary_path(finalPath, temporaryPath)) {
            return false;
        }
        const HANDLE raw = CreateFileW(temporaryPath.chars.data(),
                                       GENERIC_WRITE,
                                       0,
                                       nullptr,
                                       CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                       nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            return false;
        }
        FileHandle file(raw);
        bool complete = write_all(file.get(), bytes);
        complete = file.close() && complete;
        if (!complete) {
            (void)DeleteFileW(temporaryPath.chars.data());
        }
        return complete;
    }
    return false;
}

/**
 * Reopens and authenticates a complete sibling before publication.
 * @param path Null-terminated sibling path.
 * @param identity Expected generated-source identities.
 * @param payloadSha256 Receives the authenticated payload digest.
 * @return True when the mapped file reproduces the complete envelope and digest.
 */
[[nodiscard]] bool
validate_file_image(const wchar_t* path, const Identity& identity, Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    const HANDLE raw = CreateFileW(path,
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_DELETE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                                   nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return false;
    }
    FileHandle file(raw);
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0
        || static_cast<std::uint64_t>(size.QuadPart)
               > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    const HANDLE mapping = CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        return false;
    }
    const void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    const bool valid = view != nullptr
                       && validate(std::span(static_cast<const std::byte*>(view),
                                             static_cast<std::size_t>(size.QuadPart)),
                                   identity,
                                   payloadSha256);
    if (view != nullptr) {
        (void)UnmapViewOfFile(view);
    }
    (void)CloseHandle(mapping);
    return valid;
}

} // namespace

/**
 * Validates the complete canonical envelope and hashes its exact payload bytes.
 * @param bytes Complete header and payload bytes.
 * @param identity Expected generated-source identities.
 * @param payloadSha256 Receives the verified payload digest only on success.
 * @return True when the envelope and payload are exact.
 */
bool validate(std::span<const std::byte> bytes,
              const Identity& identity,
              Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    if (bytes.size() < sizeof(format::Header)) {
        return false;
    }
    format::Header header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    if (!valid_shape(header, bytes.size()) || header.sdkBuildSha256 != identity.sdkBuildSha256
        || header.contentKeySha256 != identity.contentKeySha256
        || header.logicalIrSha256 != identity.logicalIrSha256) {
        return false;
    }
    Digest digest{};
    if (!middleware::crypto::sha256::hash(bytes.subspan(header.headerSize), digest)
        || digest != header.payloadSha256) {
        return false;
    }
    payloadSha256 = digest;
    return true;
}

/** Reopens and authenticates one complete canonical pack. */
bool validate_file(const wchar_t* path, const Identity& identity, Digest& payloadSha256) noexcept {
    return validate_file_image(path, identity, payloadSha256);
}

/** Builds and hashes one identity-independent canonical image exactly once. */
Status prepare(const Tables& tables, PreparedImage& output) noexcept {
    try {
        const auto inputs = raw_sections(tables);
        format::Header header{};
        header.magic = format::kMagic;
        header.version = format::kVersion;
        header.headerSize = static_cast<std::uint32_t>(sizeof(format::Header));
        header.sectionCount = format::kSectionCount;

        std::size_t fileSize = 0;
        const Status described = describe_sections(inputs, header, fileSize);
        if (described != Status::ready) {
            return described;
        }
        header.fileSize = fileSize;
        PreparedImage pending{};
        pending.bytes_.resize(fileSize);
        copy_sections(inputs, header, pending.bytes_);
        if (!middleware::crypto::sha256::hash(
                std::span<const std::byte>(pending.bytes_).subspan(header.headerSize),
                header.payloadSha256)) {
            return Status::hashFailure;
        }
        std::memcpy(pending.bytes_.data(), &header, sizeof header);
        pending.payloadSha256_ = header.payloadSha256;
        output = std::move(pending);
        return Status::ready;
    } catch (...) {
        return Status::sizeOverflow;
    }
}

/** Binds the final header identity and publishes the already-prepared image without rereading. */
Status publish(const wchar_t* path,
               const Identity& identity,
               PreparedImage prepared,
               Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    if (path == nullptr || path[0] == L'\0' || prepared.bytes_.size() < sizeof(format::Header)) {
        return Status::invalidInput;
    }
    format::Header header{};
    std::memcpy(&header, prepared.bytes_.data(), sizeof header);
    if (header.magic != format::kMagic || header.version != format::kVersion
        || header.headerSize != sizeof(format::Header) || header.fileSize != prepared.bytes_.size()
        || header.sectionCount != format::kSectionCount
        || header.payloadSha256 != prepared.payloadSha256_) {
        return Status::invalidInput;
    }
    header.sdkBuildSha256 = identity.sdkBuildSha256;
    header.contentKeySha256 = identity.contentKeySha256;
    header.logicalIrSha256 = identity.logicalIrSha256;
    std::memcpy(prepared.bytes_.data(), &header, sizeof header);

    core::path::Buffer temporaryPath;
    if (!write_temporary(path, prepared.bytes_, temporaryPath)) {
        return Status::ioFailure;
    }
    if (!core::path::publish_sibling(temporaryPath.chars.data(), path)) {
        (void)DeleteFileW(temporaryPath.chars.data());
        return Status::ioFailure;
    }
    payloadSha256 = prepared.payloadSha256_;
    return Status::ready;
}

/**
 * Builds one canonical header and copies caller-owned rows without translation.
 * @param identity Exact generated-source identities.
 * @param tables Canonical string bytes and fixed-layout rows.
 * @param output Replaced only with a complete deterministic file image.
 * @param payloadSha256 Receives the payload digest only on success.
 * @return Precise build outcome.
 */
Status build(const Identity& identity,
             const Tables& tables,
             std::vector<std::byte>& output,
             Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    PreparedImage prepared{};
    const Status status = prepare(tables, prepared);
    if (status != Status::ready) {
        return status;
    }
    format::Header header{};
    std::memcpy(&header, prepared.bytes_.data(), sizeof header);
    header.sdkBuildSha256 = identity.sdkBuildSha256;
    header.contentKeySha256 = identity.contentKeySha256;
    header.logicalIrSha256 = identity.logicalIrSha256;
    std::memcpy(prepared.bytes_.data(), &header, sizeof header);
    payloadSha256 = prepared.payloadSha256_;
    output = std::move(prepared.bytes_);
    return Status::ready;
}

/**
 * Publishes one deterministic image through a closed writer-owned sibling.
 * @param path Null-terminated final pack path.
 * @param identity Exact generated-source identities.
 * @param tables Canonical string bytes and fixed-layout rows.
 * @param payloadSha256 Receives the committed payload digest only on success.
 * @return Precise build or publication outcome.
 */
Status write(const wchar_t* path,
             const Identity& identity,
             const Tables& tables,
             Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    if (path == nullptr || path[0] == L'\0') {
        return Status::invalidInput;
    }
    PreparedImage prepared{};
    const Status built = prepare(tables, prepared);
    return built == Status::ready ? publish(path, identity, std::move(prepared), payloadSha256)
                                  : built;
}

} // namespace sunrise::state::activity_sdk::generation::pack
