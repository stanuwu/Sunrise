#include "activity_sdk_decode_plans.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "../../../core/filesystem/temporary_sibling.h"
#include "../../../core/logging/log.h"
#include "../../../state/gameplay/external/rsat_decode_plans.h"
#include "activity_sdk_decode_plans_internal.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans {
namespace {

using namespace internal;

/** Cache file identity: magic, version 2, the client build it was compiled against. */
constexpr std::string_view kMagic = "SRRSATP2";
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kClientBuild = 86657;
constexpr std::size_t kHeaderSize = 136;
/** Identity of this builder; its text is hashed into the evidence. */
constexpr std::string_view kGeneratorId = "sunrise-rsat-decode-plans-builder-1";
constexpr std::wstring_view kTemporarySuffix = L".tmp";
/** Per-pair refusals logged before the rest are only counted. */
constexpr std::uint32_t kLoggedErrorLimit = 16;

/** One plan row of the cache: component, selection, entry range, bitmap bits, active. */
struct PlanRow final {
    std::uint32_t component{};
    std::uint32_t schema{};
    std::uint32_t first{};
    std::uint32_t count{};
    std::uint32_t bits{};
    std::uint32_t active{};
};

/** One package codec row of the cache. */
struct ExtraRow final {
    std::uint32_t handle{};
    std::uint32_t original{};
    std::uint32_t serialized{};
    std::uint32_t flags{};
    std::uint32_t array{};
    std::uint32_t first{};
    std::uint32_t count{};
};

// Rows are copied as raw bytes, so their layouts must match the cache format exactly.
static_assert(sizeof(PlanRow) == 24 && sizeof(PlanEntry) == 20 && sizeof(SchemaRow) == 16
              && sizeof(ExtraRow) == 28);

template <typename T> void put(std::vector<std::byte>& bytes, const T& value) {
    const std::size_t at = bytes.size();
    bytes.resize(at + sizeof(T));
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}

/** Appends one 40-byte field row in cache order. */
void put_field(std::vector<std::byte>& bytes, const Field& field) {
    put(bytes, field.offset);
    put(bytes, field.alternate);
    put(bytes, field.bit);
    put(bytes, field.half);
    put(bytes, field.type);
    put(bytes, field.presence);
    put(bytes, field.parameter2);
    put(bytes, field.nested);
    put(bytes, field.bias);
    put(bytes, field.width);
    put(bytes, field.parameter3);
    put(bytes, field.parameter4);
}

void append_hex(std::string& text, std::span<const std::byte> bytes) {
    // Digests are lowercase hex in the evidence and the manifest.
    constexpr std::string_view digits = "0123456789abcdef";
    for (const std::byte value : bytes) {
        text.push_back(digits[static_cast<std::size_t>(value) >> 4U]);
        text.push_back(digits[static_cast<std::size_t>(value) & 0x0FU]);
    }
}

void append_handle(std::string& text, std::uint32_t handle) {
    std::array<char, 16> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%08X", handle);
    text.append(buffer.data(), written > 0 ? static_cast<std::size_t>(written) : 0U);
}

/** Compact JSON of the evidence in key order; its exact bytes are hashed into the fingerprint. */
void append_evidence(std::string& text,
                     const Digest& executable,
                     const Digest& generator,
                     const std::map<std::uint32_t, Digest>& packages) {
    text.append("{\"executableSha256\":\"");
    append_hex(text, executable);
    text.append("\",\"generatorSha256\":\"");
    append_hex(text, generator);
    text.append("\",\"packageSha256\":{");
    bool first = true;
    for (const auto& [handle, digest] : packages) {
        text.append(first ? "\"" : ",\"");
        first = false;
        append_handle(text, handle);
        text.append("\":\"");
        append_hex(text, digest);
        text.push_back('"');
    }
    text.append("}}");
}

/** Two-space indented manifest in key order. */
void append_manifest(std::string& text,
                     const Digest& sdk,
                     const Digest& fingerprint,
                     const Digest& executable,
                     const Digest& generator,
                     const Digest& payload,
                     const std::map<std::uint32_t, Digest>& packages) {
    text.append("{\n  \"build\": 86657,\n  \"evidenceFingerprint\": \"");
    append_hex(text, fingerprint);
    text.append("\",\n  \"executableSha256\": \"");
    append_hex(text, executable);
    text.append("\",\n  \"format\": \"SRRSATP2\",\n  \"generatorSha256\": \"");
    append_hex(text, generator);
    text.append("\",\n  \"packageSha256\": {");
    bool first = true;
    for (const auto& [handle, digest] : packages) {
        text.append(first ? "\n    \"" : ",\n    \"");
        first = false;
        append_handle(text, handle);
        text.append("\": \"");
        append_hex(text, digest);
        text.push_back('"');
    }
    text.append(first ? "},\n  \"payloadSha256\": \"" : "\n  },\n  \"payloadSha256\": \"");
    append_hex(text, payload);
    text.append("\",\n  \"sdkBuildHash\": \"");
    append_hex(text, sdk);
    text.append("\"\n}\n");
}

/**
 * Writes one complete file through a sibling so a reader never sees a partial file.
 * @return False when the sibling could not be written or published.
 */
[[nodiscard]] bool write_file(std::wstring_view finalPath,
                              std::span<const std::byte> bytes) noexcept {
    try {
        const std::wstring final(finalPath);
        const std::wstring temporary = final + std::wstring(kTemporarySuffix);
        const HANDLE file = CreateFileW(temporary.c_str(),
                                        GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD written = 0;
        const bool complete =
            WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
                != FALSE
            && written == bytes.size();
        if (CloseHandle(file) == FALSE || !complete) {
            (void)DeleteFileW(temporary.c_str());
            return false;
        }
        return core::path::publish_sibling(temporary.c_str(), final.c_str());
    } catch (...) {
        return false;
    }
}

/** Logs one refused pair; the cache leaves it out. */
void log_pair_error(std::uint32_t component, std::uint32_t schema, const char* detail) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_sdk_decoder_cache result=skip component=0x%08X "
                                      "selection=0x%08X detail=%s",
                                      component,
                                      schema,
                                      detail == nullptr ? "unknown" : detail);
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::warn,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Reads every package codec reachable from the compiled metadata into sorted rows. */
[[nodiscard]] bool collect_extras(Store& store,
                                  std::vector<ExtraRow>& rows,
                                  std::vector<Field>& fields,
                                  std::vector<SchemaRow>& schemas,
                                  std::vector<std::uint32_t>& fieldValues) {
    std::vector<std::uint32_t> pending;
    for (const auto& [handle, record] : store.entries()) {
        if (record.package) {
            pending.push_back(handle);
        }
    }
    std::map<std::uint32_t, Codec> extras;
    const char* error = nullptr;
    while (!pending.empty()) {
        const std::uint32_t handle = pending.back();
        pending.pop_back();
        if (extras.contains(handle)) {
            continue;
        }
        const Metadata* record = store.get(handle, error);
        if (record == nullptr) {
            return false;
        }
        if (!record->codec.present) {
            continue;
        }
        const Codec codec = record->codec;
        for (const Field& field : codec.fields) {
            if (field.type == kTypeNested && !store.image().slots().contains(field.nested)) {
                pending.push_back(field.nested);
            }
        }
        extras.emplace(handle, codec);
    }
    for (const auto& [handle, codec] : extras) {
        rows.push_back({handle,
                        codec.original,
                        codec.serialized,
                        codec.flags,
                        codec.array,
                        static_cast<std::uint32_t>(fields.size()),
                        static_cast<std::uint32_t>(codec.fields.size())});
        fields.insert(fields.end(), codec.fields.begin(), codec.fields.end());
        schemas.push_back({handle,
                           codec.serialized,
                           static_cast<std::uint32_t>(fieldValues.size()),
                           static_cast<std::uint32_t>(codec.fields.size())});
        for (const Field& field : codec.fields) {
            fieldValues.push_back(field.bit);
        }
    }
    return true;
}

[[nodiscard]] bool cancelled(const Request& request) noexcept {
    return request.cancel != nullptr && request.cancel(request.cancelContext);
}

/** Compiles every pair; refused pairs are counted and left out. */
[[nodiscard]] Status
compile_pairs(const Request& request, Store& store, std::vector<Plan>& plans, Result& output) {
    std::set<std::pair<std::uint32_t, std::uint32_t>> pairs;
    for (const auto& descriptor : request.descriptors) {
        if (descriptor.schemaTag != 0 && descriptor.schemaTag != kAbsent) {
            pairs.emplace(descriptor.componentTag, descriptor.schemaTag);
        }
    }
    output.pairs = static_cast<std::uint32_t>(pairs.size());
    std::vector<std::byte> blob;
    for (const auto& [component, schemaTag] : pairs) {
        if (cancelled(request)) {
            return Status::cancelled;
        }
        const char* error = nullptr;
        Digest digest{};
        Plan plan{};
        plan.schemaTag = schemaTag;
        bool complete = store.read_package(schemaTag, kSelectionClass, blob);
        if (!complete) {
            error = "selection_class";
        } else if (!middleware::crypto::sha256::hash(blob, digest)) {
            return Status::hashFailed;
        } else {
            store.package_hashes()[schemaTag] = digest;
            complete = compile(store, component, blob, plan, error);
        }
        if (!complete) {
            if (output.errors < kLoggedErrorLimit) {
                log_pair_error(component, schemaTag, error);
            }
            ++output.errors;
            continue;
        }
        plans.push_back(std::move(plan));
    }
    return Status::ready;
}

/** Owns the package reader storage for one build. */
struct ScratchOwner final {
    std::unique_ptr<reader::Scratch> value{new (std::nothrow) reader::Scratch()};
    ~ScratchOwner() noexcept {
        if (value != nullptr) {
            reader::close_files(*value);
        }
    }
};

/**
 * Compiles the plans, rows the codec index and writes the cache pair.
 * @param request Validated build inputs.
 * @param output Receives the counts as they are known, even on failure.
 * @return Status::ready when both files are on disk.
 */
[[nodiscard]] Status run(const Request& request, Result& output) {
    const Image& image = *request.image;
    output.slots = static_cast<std::uint32_t>(image.slots().size());
    ScratchOwner scratch;
    if (scratch.value == nullptr) {
        return Status::allocation;
    }
    Store store(image, *request.source, *scratch.value);
    std::vector<Plan> plans;
    const Status compiled = compile_pairs(request, store, plans, output);
    if (compiled != Status::ready) {
        return compiled;
    }
    std::vector<SchemaRow> schemas;
    std::vector<std::uint32_t> fieldValues;
    const char* error = nullptr;
    if (!build_codec_index(image, schemas, fieldValues, error)) {
        return Status::reflectionInvalid;
    }
    std::vector<ExtraRow> extraRows;
    std::vector<Field> extraFields;
    if (!collect_extras(store, extraRows, extraFields, schemas, fieldValues)) {
        return Status::packageRead;
    }
    std::sort(schemas.begin(), schemas.end(), [](const SchemaRow& one, const SchemaRow& other) {
        return std::tie(one.handle, one.size, one.first, one.count)
               < std::tie(other.handle, other.size, other.first, other.count);
    });
    std::vector<PlanRow> rows;
    std::vector<PlanEntry> entries;
    for (const Plan& plan : plans) {
        rows.push_back({plan.component,
                        plan.schemaTag,
                        static_cast<std::uint32_t>(entries.size()),
                        static_cast<std::uint32_t>(plan.entries.size()),
                        plan.bitmapBits,
                        plan.active ? 1U : 0U});
        entries.insert(entries.end(), plan.entries.begin(), plan.entries.end());
        output.active += plan.active ? 1U : 0U;
    }
    output.plans = static_cast<std::uint32_t>(rows.size());
    output.entries = static_cast<std::uint32_t>(entries.size());
    output.schemas = static_cast<std::uint32_t>(schemas.size());
    output.fields = static_cast<std::uint32_t>(fieldValues.size());

    std::vector<std::byte> payload;
    for (const PlanRow& row : rows) {
        put(payload, row);
    }
    for (const PlanEntry& entry : entries) {
        put(payload, entry);
    }
    for (const SchemaRow& row : schemas) {
        put(payload, row);
    }
    for (const std::uint32_t value : fieldValues) {
        put(payload, value);
    }
    for (const ExtraRow& row : extraRows) {
        put(payload, row);
    }
    for (const Field& field : extraFields) {
        put_field(payload, field);
    }

    Digest executable{};
    Digest generator{};
    Digest payloadDigest{};
    Digest fingerprint{};
    const std::span<const std::byte> generatorText(
        reinterpret_cast<const std::byte*>(kGeneratorId.data()), kGeneratorId.size());
    if (!middleware::crypto::sha256::hash(image.file(), executable)
        || !middleware::crypto::sha256::hash(generatorText, generator)
        || !middleware::crypto::sha256::hash(payload, payloadDigest)) {
        return Status::hashFailed;
    }
    std::string evidence;
    append_evidence(evidence, executable, generator, store.package_hashes());
    if (!middleware::crypto::sha256::hash(
            std::span(reinterpret_cast<const std::byte*>(evidence.data()), evidence.size()),
            fingerprint)) {
        return Status::hashFailed;
    }
    Digest sdk{};
    std::copy(request.sdkBuildSha256.begin(), request.sdkBuildSha256.end(), sdk.begin());

    std::vector<std::byte> cache;
    cache.reserve(kHeaderSize + payload.size());
    cache.insert(cache.end(),
                 reinterpret_cast<const std::byte*>(kMagic.data()),
                 reinterpret_cast<const std::byte*>(kMagic.data()) + kMagic.size());
    put(cache, kVersion);
    put(cache, kClientBuild);
    cache.insert(cache.end(), sdk.begin(), sdk.end());
    cache.insert(cache.end(), fingerprint.begin(), fingerprint.end());
    cache.insert(cache.end(), payloadDigest.begin(), payloadDigest.end());
    put(cache, static_cast<std::uint32_t>(rows.size()));
    put(cache, static_cast<std::uint32_t>(entries.size()));
    put(cache, static_cast<std::uint32_t>(schemas.size()));
    put(cache, static_cast<std::uint32_t>(fieldValues.size()));
    put(cache, static_cast<std::uint32_t>(extraRows.size()));
    put(cache, static_cast<std::uint32_t>(extraFields.size()));
    cache.insert(cache.end(), payload.begin(), payload.end());

    std::string manifest;
    append_manifest(
        manifest, sdk, fingerprint, executable, generator, payloadDigest, store.package_hashes());
    const std::wstring manifestPath =
        std::wstring(request.cachePath)
        + std::wstring(state::gameplay::rsat_decode_plans::kManifestSuffix);
    if (!write_file(request.cachePath, cache)
        || !write_file(
            manifestPath,
            std::span(reinterpret_cast<const std::byte*>(manifest.data()), manifest.size()))) {
        return Status::writeFailed;
    }
    return Status::ready;
}

} // namespace

/**
 * Checks the request, then runs the build with allocation failures mapped to a status.
 * @param request Executable, package source, descriptor pairs, SDK identity and output path.
 * @param output Receives the counts of the written cache.
 * @return Status::ready when both files are complete on disk.
 */
Status build(const Request& request, Result& output) noexcept {
    output = {};
    if (request.image == nullptr || request.source == nullptr || request.cachePath.empty()
        || request.sdkBuildSha256.size() != Digest{}.size()) {
        return Status::invalidInput;
    }
    try {
        return run(request, output);
    } catch (...) {
        return Status::allocation;
    }
}

/** @return The stable log name of one build status. */
const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ready:
        return "ready";
    case Status::cancelled:
        return "cancelled";
    case Status::invalidInput:
        return "invalid_input";
    case Status::executableUnavailable:
        return "executable_unavailable";
    case Status::executableInvalid:
        return "executable_invalid";
    case Status::reflectionInvalid:
        return "reflection_invalid";
    case Status::packageRead:
        return "package_read";
    case Status::hashFailed:
        return "hash_failed";
    case Status::writeFailed:
        return "write_failed";
    case Status::allocation:
        return "allocation";
    }
    return "unknown";
}

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans
