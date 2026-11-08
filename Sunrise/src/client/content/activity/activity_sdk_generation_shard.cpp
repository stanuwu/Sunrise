#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <span>

#include "../../../state/activity_sdk/generated_world/store.h"
#include "activity_sdk_generation_worker_internal.h"

namespace sunrise::client::content::activity::sdk_generation::worker_internal {

namespace shard_store = state::activity_sdk::generated_world::store;

// Shards are published with the pack extension.
constexpr std::wstring_view kShardExtension = L".pack";

/** Formats an immutable shard path from its scenario and payload identities. */
[[nodiscard]] bool shard_path(const std::wstring& directory,
                              std::uint32_t scenarioTag,
                              const generated_world::Digest& digest,
                              std::wstring& output) noexcept {
    // A shard leaf spells the digest in lowercase hex.
    static constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::array<wchar_t, 10> tag{};
    const int length =
        std::swprintf(tag.data(), tag.size(), L"%08X-", static_cast<unsigned>(scenarioTag));
    if (length != 9 || directory.empty()) {
        return false;
    }
    try {
        output = directory;
        output.push_back(L'\\');
        output.append(tag.data(), static_cast<std::size_t>(length));
        for (const std::byte byte : digest) {
            const unsigned value = std::to_integer<unsigned>(byte);
            output.push_back(kDigits[(value >> 4U) & 0xFU]);
            output.push_back(kDigits[value & 0xFU]);
        }
        output.append(kShardExtension);
    } catch (...) {
        output.clear();
        return false;
    }
    return true;
}

/** Copies one authenticated full shard into caller-owned storage. */
[[nodiscard]] bool load_record(const std::wstring& scenarioDirectory,
                               const generated_world::Digest& sourceFingerprint,
                               const Scenario& scenario,
                               const manifest::Record& record,
                               catalog::Snapshot& output) noexcept {
    std::shared_ptr<const catalog::Snapshot> loaded;
    shard_store::RecordLoadStatus status = shard_store::RecordLoadStatus::invalid;
    if (!shard_store::load_record(scenarioDirectory,
                                  scenario.tag,
                                  std::string_view(scenario.name.data(), scenario.nameLength),
                                  sourceFingerprint,
                                  record,
                                  loaded,
                                  status)
        || status != shard_store::RecordLoadStatus::loaded || loaded == nullptr) {
        return false;
    }
    try {
        output = *loaded;
    } catch (...) {
        output = {};
        return false;
    }
    return true;
}

/** Reopens one generator-owned row only when every full extraction family is present. */
[[nodiscard]] bool load_full_record(const std::wstring& scenarioDirectory,
                                    const generated_world::Digest& sourceFingerprint,
                                    const Scenario& scenario,
                                    const manifest::Record& record,
                                    std::shared_ptr<const catalog::Snapshot>& output) noexcept {
    shard_store::RecordLoadStatus status = shard_store::RecordLoadStatus::invalid;
    return shard_store::load_record(scenarioDirectory,
                                    scenario.tag,
                                    std::string_view(scenario.name.data(), scenario.nameLength),
                                    sourceFingerprint,
                                    record,
                                    output,
                                    status)
           && status == shard_store::RecordLoadStatus::loaded;
}

/** Finds one sorted manifest record by scenario tag. */
[[nodiscard]] const manifest::Record* find_record(const manifest::Catalog& catalog,
                                                  std::uint32_t scenarioTag) noexcept {
    const auto found = std::lower_bound(
        catalog.records.begin(),
        catalog.records.end(),
        scenarioTag,
        [](const manifest::Record& row, std::uint32_t tag) { return row.scenarioTag < tag; });
    return found != catalog.records.end() && found->scenarioTag == scenarioTag ? &*found : nullptr;
}

/** Accepts the requested name or this generator's exact unnamed-scenario fallback. */
[[nodiscard]] bool record_name_matches(const manifest::Record& record,
                                       std::uint32_t scenarioTag,
                                       std::string_view requestedName) noexcept {
    const std::string_view recordName(record.scenarioName.data(), record.scenarioNameLength);
    if (recordName == requestedName) {
        return true;
    }
    std::array<char, 32> synthetic{};
    const int length = std::snprintf(
        synthetic.data(), synthetic.size(), "scenario_%08X", static_cast<unsigned>(scenarioTag));
    return length > 0 && static_cast<std::size_t>(length) < synthetic.size()
           && recordName == std::string_view(synthetic.data(), static_cast<std::size_t>(length));
}

} // namespace sunrise::client::content::activity::sdk_generation::worker_internal
