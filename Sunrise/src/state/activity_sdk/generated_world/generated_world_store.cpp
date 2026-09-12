#include <algorithm>
#include <array>
#include <cwchar>

#include "../../build_data/scriptables/coverage.h"
#include "store.h"

namespace sunrise::state::activity_sdk::generated_world::store {
namespace {

namespace catalog = build_data::scriptables;

/** @return True when one digest carries an identity instead of the failure value. */
[[nodiscard]] bool nonzero(const Digest& value) noexcept {
    return value != Digest{};
}

/** Checks a fixed-width manifest name against one caller-owned exact name. */
[[nodiscard]] bool record_name_matches(const manifest::Record& record,
                                       std::string_view expected) noexcept {
    return record.scenarioNameLength == expected.size()
           && std::equal(expected.begin(), expected.end(), record.scenarioName.begin());
}

/** Maps the codec refusal without losing stale-source or missing-file detail. */
[[nodiscard]] RecordLoadStatus map_status(LoadStatus value) noexcept {
    switch (value) {
    case LoadStatus::loaded:
        return RecordLoadStatus::loaded;
    case LoadStatus::missing:
        return RecordLoadStatus::missing;
    case LoadStatus::scenarioMismatch:
        return RecordLoadStatus::scenarioMismatch;
    case LoadStatus::sourceMismatch:
        return RecordLoadStatus::sourceMismatch;
    case LoadStatus::versionMismatch:
    case LoadStatus::invalid:
        return RecordLoadStatus::invalid;
    }
    return RecordLoadStatus::invalid;
}

} // namespace

/** @return The stable log name of one record load status. */
const char* status_name(RecordLoadStatus value) noexcept {
    switch (value) {
    case RecordLoadStatus::loaded:
        return "loaded";
    case RecordLoadStatus::invalidIdentity:
        return "invalid_identity";
    case RecordLoadStatus::scenarioMismatch:
        return "scenario_mismatch";
    case RecordLoadStatus::missing:
        return "missing";
    case RecordLoadStatus::sourceMismatch:
        return "source_mismatch";
    case RecordLoadStatus::payloadMismatch:
        return "payload_mismatch";
    case RecordLoadStatus::invalid:
        return "invalid";
    }
    return "invalid";
}

const manifest::Record* find_record(std::span<const manifest::Record> records,
                                    std::uint32_t scenarioTag) noexcept {
    const auto found = std::lower_bound(
        records.begin(),
        records.end(),
        scenarioTag,
        [](const manifest::Record& row, std::uint32_t tag) { return row.scenarioTag < tag; });
    return found != records.end() && found->scenarioTag == scenarioTag ? &*found : nullptr;
}

/** Builds the on-disk path of one shard from its scenario tag and payload digest. */
bool shard_path(std::wstring_view scenarioDirectory,
                std::uint32_t scenarioTag,
                const Digest& payloadSha256,
                core::path::Buffer& output) noexcept {
    // The shard filename spells its payload digest in lowercase hex.
    static constexpr std::array<wchar_t, 16> kDigits{
        L'0',
        L'1',
        L'2',
        L'3',
        L'4',
        L'5',
        L'6',
        L'7',
        L'8',
        L'9',
        L'a',
        L'b',
        L'c',
        L'd',
        L'e',
        L'f',
    };
    output = {};
    if (scenarioDirectory.empty() || scenarioTag == 0 || !nonzero(payloadSha256)) {
        return false;
    }
    std::array<wchar_t, 10> prefix{};
    const int length =
        std::swprintf(prefix.data(), prefix.size(), L"%08X-", static_cast<unsigned>(scenarioTag));
    if (length != 9 || !core::path::assign(output, scenarioDirectory)) {
        return false;
    }
    const wchar_t last = output.chars[output.length - 1];
    if (last != L'\\' && last != L'/') {
        if (!core::path::append(output, L"\\")) {
            return false;
        }
    }
    std::array<wchar_t, 2 * std::tuple_size_v<Digest>> hex{};
    std::size_t written = 0;
    for (const std::byte byte : payloadSha256) {
        const unsigned value = std::to_integer<unsigned>(byte);
        hex[written++] = kDigits[(value >> 4U) & 0xFU];
        hex[written++] = kDigits[value & 0xFU];
    }
    return core::path::append(output,
                              std::wstring_view(prefix.data(), static_cast<std::size_t>(length)))
           && core::path::append(output, std::wstring_view(hex.data(), written))
           && core::path::append(output, L".pack");
}

/** Loads and authenticates one manifest record's shard. @param status Receives the outcome. */
bool load_record(std::wstring_view scenarioDirectory,
                 std::uint32_t expectedScenarioTag,
                 std::string_view expectedScenarioName,
                 const Digest& expectedSourceFingerprint,
                 const manifest::Record& record,
                 std::shared_ptr<const catalog::Snapshot>& output,
                 RecordLoadStatus& status) noexcept {
    output.reset();
    status = RecordLoadStatus::invalid;
    if (expectedScenarioTag == 0 || expectedScenarioName.empty()
        || expectedScenarioName.size() >= catalog::kScenarioNameCapacity
        || !nonzero(expectedSourceFingerprint) || !nonzero(record.shardPayloadSha256)) {
        status = RecordLoadStatus::invalidIdentity;
        return false;
    }
    if (record.scenarioTag != expectedScenarioTag
        || !record_name_matches(record, expectedScenarioName)) {
        status = RecordLoadStatus::scenarioMismatch;
        return false;
    }

    core::path::Buffer path;
    if (!shard_path(scenarioDirectory, expectedScenarioTag, record.shardPayloadSha256, path)) {
        status = RecordLoadStatus::invalidIdentity;
        return false;
    }
    std::shared_ptr<catalog::Snapshot> pending;
    try {
        pending = std::make_shared<catalog::Snapshot>();
    } catch (...) {
        return false;
    }
    Digest payload{};
    LoadStatus loadStatus = LoadStatus::invalid;
    if (!generated_world::load(path.chars.data(),
                               expectedScenarioTag,
                               expectedSourceFingerprint,
                               *pending,
                               payload,
                               loadStatus)) {
        status = map_status(loadStatus);
        return false;
    }
    if (payload != record.shardPayloadSha256) {
        status = RecordLoadStatus::payloadMismatch;
        return false;
    }
    if (pending->scenarioTag != expectedScenarioTag
        || pending->scenarioNameLength != expectedScenarioName.size()
        || !std::equal(expectedScenarioName.begin(),
                       expectedScenarioName.end(),
                       pending->scenarioName.begin())) {
        status = RecordLoadStatus::scenarioMismatch;
        return false;
    }
    // decode_payload only accepts full-coverage snapshots, so coverage needs no second check.
    output = std::move(pending);
    status = RecordLoadStatus::loaded;
    return true;
}

/**
 * Checks published shard headers before startup reuses a loaded core catalog.
 * @param manifestPath Published catalog file.
 * @param scenarioDirectory Directory of its immutable shard files.
 * @param expectedSourceFingerprint Installed content identity.
 * @param expectedSdk Already-loaded core catalog identity.
 * @return True when every declared shard has a compatible manifest-owned header.
 */
bool compatible_catalog(const wchar_t* manifestPath,
                        std::wstring_view scenarioDirectory,
                        const Digest& expectedSourceFingerprint,
                        const manifest::SdkIdentity& expectedSdk) noexcept {
    manifest::Catalog catalog{};
    manifest::LoadStatus manifestStatus = manifest::LoadStatus::invalid;
    if (!manifest::load(
            manifestPath, expectedSourceFingerprint, expectedSdk, catalog, manifestStatus)
        || catalog.records.empty()) {
        return false;
    }
    for (const manifest::Record& record : catalog.records) {
        core::path::Buffer path;
        if (!shard_path(scenarioDirectory, record.scenarioTag, record.shardPayloadSha256, path)
            || !generated_world::compatible_header(path.chars.data(),
                                                   record.scenarioTag,
                                                   expectedSourceFingerprint,
                                                   record.shardPayloadSha256)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::state::activity_sdk::generated_world::store
