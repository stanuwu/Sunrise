#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include "../../content_manifest/content_manifest_state_runtime.h"
#include "runtime.h"

namespace sunrise::state::activity_sdk::generated_world {
namespace {

namespace catalog = build_data::scriptables;

// Retained shards, and the two generated-world paths built under the SDK artifact directory.
constexpr std::size_t kCacheCapacity = 8;
constexpr std::wstring_view kManifestSuffix = L"\\sdk\\catalog.bin";
constexpr std::wstring_view kScenarioDirectorySuffix = L"\\sdk\\scenarios";

/** One decoded immutable shard retained by its complete authenticated identity. */
struct CacheEntry final {
    core::path::Buffer scenarioDirectory{};
    std::uint32_t scenarioTag{};
    std::array<char, catalog::kScenarioNameCapacity> scenarioName{};
    std::uint8_t scenarioNameLength{};
    Digest sdkBuildSha256{};
    Digest sdkPayloadSha256{};
    Digest sourceFingerprint{};
    Digest manifestPayloadSha256{};
    Digest shardPayloadSha256{};
    std::shared_ptr<const catalog::Snapshot> snapshot{};
};

/** One internal result copied into the public view only for ready or core-only outcomes. */
struct Resolved final {
    std::uint32_t scenarioTag{};
    std::string_view scenarioName{};
    Digest sourceFingerprint{};
    Digest manifestPayloadSha256{};
    Digest shardPayloadSha256{};
    std::shared_ptr<const manifest::Catalog> manifestCatalog{};
    std::shared_ptr<const catalog::Snapshot> snapshot{};
};

SRWLOCK g_cacheLock{SRWLOCK_INIT};
std::array<CacheEntry, kCacheCapacity> g_cache{};
std::size_t g_nextCacheEntry{};

/** Checks the structural identities that make one public BoundView exact. */
[[nodiscard]] bool exact_bound_view(const activity_sdk::BoundView& view,
                                    const format::Scenario*& scenario,
                                    std::string_view& scenarioName) noexcept {
    scenario = activity_sdk::bound_scenario(view);
    const format::Activity* const activity = activity_sdk::bound_activity(view);
    if (activity == nullptr || scenario == nullptr || view.activityClientGeneration == 0
        || activity->scenarioIndex != view.scenarioRow
        || (activity->flags & format::kActivityExactMask) != format::kActivityExactMask
        || scenario->tag == 0 || scenario->tag == format::kAbsentIndex
        || !activity_sdk::destination_matches(*view.catalog, *activity, view.binding.destination)) {
        return false;
    }
    scenarioName = view.catalog->string(scenario->name);
    return !scenarioName.empty() && scenarioName.size() < catalog::kScenarioNameCapacity;
}

/** Joins one owned directory and suffix without changing either input. */
[[nodiscard]] bool joined_path(std::wstring_view directory,
                               std::wstring_view suffix,
                               core::path::Buffer& output) noexcept {
    output = {};
    if (directory.empty() || suffix.empty()) {
        return false;
    }
    return core::path::assign(output, directory) && core::path::append(output, suffix);
}

/** @return The stored path text, without its terminator. */
[[nodiscard]] std::wstring_view path_view(const core::path::Buffer& value) noexcept {
    return {value.chars.data(), value.length};
}

/** Checks every cache key field without trusting only a content-addressed filename. */
[[nodiscard]] bool cache_key_matches(const CacheEntry& entry,
                                     std::wstring_view scenarioDirectory,
                                     std::uint32_t scenarioTag,
                                     std::string_view scenarioName,
                                     const Digest& sdkBuildSha256,
                                     const Digest& sdkPayloadSha256,
                                     const Digest& sourceFingerprint,
                                     const Digest& manifestPayloadSha256,
                                     const Digest& shardPayloadSha256) noexcept {
    return entry.snapshot != nullptr && path_view(entry.scenarioDirectory) == scenarioDirectory
           && entry.scenarioTag == scenarioTag && entry.scenarioNameLength == scenarioName.size()
           && std::equal(scenarioName.begin(), scenarioName.end(), entry.scenarioName.begin())
           && entry.sdkBuildSha256 == sdkBuildSha256 && entry.sdkPayloadSha256 == sdkPayloadSha256
           && entry.sourceFingerprint == sourceFingerprint
           && entry.manifestPayloadSha256 == manifestPayloadSha256
           && entry.shardPayloadSha256 == shardPayloadSha256;
}

/** Returns a retained decoded shard without reopening its content-addressed file. */
[[nodiscard]] std::shared_ptr<const catalog::Snapshot>
find_cached(std::wstring_view scenarioDirectory,
            std::uint32_t scenarioTag,
            std::string_view scenarioName,
            const Digest& sdkBuildSha256,
            const Digest& sdkPayloadSha256,
            const Digest& sourceFingerprint,
            const Digest& manifestPayloadSha256,
            const Digest& shardPayloadSha256) noexcept {
    std::shared_ptr<const catalog::Snapshot> output;
    AcquireSRWLockShared(&g_cacheLock);
    for (const CacheEntry& entry : g_cache) {
        if (cache_key_matches(entry,
                              scenarioDirectory,
                              scenarioTag,
                              scenarioName,
                              sdkBuildSha256,
                              sdkPayloadSha256,
                              sourceFingerprint,
                              manifestPayloadSha256,
                              shardPayloadSha256)) {
            output = entry.snapshot;
            break;
        }
    }
    ReleaseSRWLockShared(&g_cacheLock);
    return output;
}

/** Retains one decoded shard in a fixed FIFO without shortening existing view lifetimes. */
void retain_cached(std::wstring_view scenarioDirectory,
                   std::uint32_t scenarioTag,
                   std::string_view scenarioName,
                   const Digest& sdkBuildSha256,
                   const Digest& sdkPayloadSha256,
                   const Digest& sourceFingerprint,
                   const Digest& manifestPayloadSha256,
                   const Digest& shardPayloadSha256,
                   std::shared_ptr<const catalog::Snapshot>& snapshot) noexcept {
    AcquireSRWLockExclusive(&g_cacheLock);
    for (const CacheEntry& entry : g_cache) {
        if (cache_key_matches(entry,
                              scenarioDirectory,
                              scenarioTag,
                              scenarioName,
                              sdkBuildSha256,
                              sdkPayloadSha256,
                              sourceFingerprint,
                              manifestPayloadSha256,
                              shardPayloadSha256)) {
            snapshot = entry.snapshot;
            ReleaseSRWLockExclusive(&g_cacheLock);
            return;
        }
    }
    CacheEntry& slot = g_cache[g_nextCacheEntry];
    if (scenarioDirectory.size() < slot.scenarioDirectory.chars.size()) {
        slot = {};
        (void)core::path::assign(slot.scenarioDirectory, scenarioDirectory);
        slot.scenarioTag = scenarioTag;
        slot.scenarioNameLength = static_cast<std::uint8_t>(scenarioName.size());
        std::copy(scenarioName.begin(), scenarioName.end(), slot.scenarioName.begin());
        slot.sdkBuildSha256 = sdkBuildSha256;
        slot.sdkPayloadSha256 = sdkPayloadSha256;
        slot.sourceFingerprint = sourceFingerprint;
        slot.manifestPayloadSha256 = manifestPayloadSha256;
        slot.shardPayloadSha256 = shardPayloadSha256;
        slot.snapshot = snapshot;
        g_nextCacheEntry = (g_nextCacheEntry + 1U) % g_cache.size();
    }
    ReleaseSRWLockExclusive(&g_cacheLock);
}

/** Maps one authenticated manifest refusal to the binding-level vocabulary. */
[[nodiscard]] BindStatus map_manifest_status(manifest::LoadStatus value) noexcept {
    switch (value) {
    case manifest::LoadStatus::loaded:
        return BindStatus::ready;
    case manifest::LoadStatus::missing:
        return BindStatus::manifestMissing;
    case manifest::LoadStatus::sourceMismatch:
        return BindStatus::manifestStale;
    case manifest::LoadStatus::sdkMismatch:
        return BindStatus::manifestSdkMismatch;
    case manifest::LoadStatus::versionMismatch:
    case manifest::LoadStatus::invalid:
        return BindStatus::manifestInvalid;
    }
    return BindStatus::manifestInvalid;
}

/** Maps one exact shard refusal without turning missing or stale data into corruption. */
[[nodiscard]] BindStatus map_record_status(store::RecordLoadStatus value) noexcept {
    switch (value) {
    case store::RecordLoadStatus::loaded:
        return BindStatus::ready;
    case store::RecordLoadStatus::invalidIdentity:
        return BindStatus::pathUnavailable;
    case store::RecordLoadStatus::scenarioMismatch:
        return BindStatus::scenarioMismatch;
    case store::RecordLoadStatus::missing:
        return BindStatus::shardMissing;
    case store::RecordLoadStatus::sourceMismatch:
        return BindStatus::shardStale;
    case store::RecordLoadStatus::payloadMismatch:
    case store::RecordLoadStatus::invalid:
        return BindStatus::shardInvalid;
    }
    return BindStatus::shardInvalid;
}

/** Loads one exact manifest row and its immutable shard after the BoundView identity closes. */
[[nodiscard]] BindStatus resolve_paths(const activity_sdk::BoundView& activitySdkView,
                                       const wchar_t* manifestPath,
                                       const wchar_t* scenarioDirectory,
                                       const Digest& sourceFingerprint,
                                       Resolved& output) noexcept {
    output = {};
    const format::Scenario* scenario = nullptr;
    std::string_view scenarioName;
    if (!exact_bound_view(activitySdkView, scenario, scenarioName)) {
        return BindStatus::invalidBoundView;
    }
    if (manifestPath == nullptr || manifestPath[0] == L'\0' || scenarioDirectory == nullptr
        || scenarioDirectory[0] == L'\0' || sourceFingerprint == Digest{}) {
        return BindStatus::pathUnavailable;
    }
    const auto sdkBuild = activitySdkView.catalog->sdk_build_sha256();
    const auto sdkPayload = activitySdkView.catalog->payload_sha256();
    if (sdkBuild.size() != Digest{}.size() || sdkPayload.size() != Digest{}.size()) {
        return BindStatus::invalidBoundView;
    }
    Digest sdkBuildSha256{};
    Digest sdkPayloadSha256{};
    std::copy(sdkBuild.begin(), sdkBuild.end(), sdkBuildSha256.begin());
    std::copy(sdkPayload.begin(), sdkPayload.end(), sdkPayloadSha256.begin());
    const manifest::SdkIdentity expectedSdk{sdkBuildSha256, sdkPayloadSha256};

    std::shared_ptr<manifest::Catalog> manifestCatalog;
    try {
        manifestCatalog = std::make_shared<manifest::Catalog>();
    } catch (...) {
        return BindStatus::manifestInvalid;
    }
    manifest::LoadStatus manifestStatus = manifest::LoadStatus::invalid;
    if (!manifest::load(
            manifestPath, sourceFingerprint, expectedSdk, *manifestCatalog, manifestStatus)) {
        return map_manifest_status(manifestStatus);
    }
    const manifest::Record* const record =
        store::find_record(manifestCatalog->records, scenario->tag);
    if (record == nullptr) {
        return BindStatus::scenarioMissing;
    }
    if (record->scenarioNameLength != scenarioName.size()
        || !std::equal(scenarioName.begin(), scenarioName.end(), record->scenarioName.begin())) {
        return BindStatus::scenarioMismatch;
    }

    std::shared_ptr<const catalog::Snapshot> snapshot = find_cached(scenarioDirectory,
                                                                    scenario->tag,
                                                                    scenarioName,
                                                                    sdkBuildSha256,
                                                                    sdkPayloadSha256,
                                                                    sourceFingerprint,
                                                                    manifestCatalog->payloadSha256,
                                                                    record->shardPayloadSha256);
    if (snapshot == nullptr) {
        store::RecordLoadStatus recordStatus = store::RecordLoadStatus::invalid;
        if (!store::load_record(scenarioDirectory,
                                scenario->tag,
                                scenarioName,
                                sourceFingerprint,
                                *record,
                                snapshot,
                                recordStatus)) {
            return map_record_status(recordStatus);
        }
        retain_cached(scenarioDirectory,
                      scenario->tag,
                      scenarioName,
                      sdkBuildSha256,
                      sdkPayloadSha256,
                      sourceFingerprint,
                      manifestCatalog->payloadSha256,
                      record->shardPayloadSha256,
                      snapshot);
    }

    output.scenarioTag = scenario->tag;
    output.scenarioName = scenarioName;
    output.sourceFingerprint = sourceFingerprint;
    output.manifestPayloadSha256 = manifestCatalog->payloadSha256;
    output.shardPayloadSha256 = record->shardPayloadSha256;
    output.manifestCatalog = std::move(manifestCatalog);
    output.snapshot = std::move(snapshot);
    // The shard loader only returns full-coverage snapshots, so a loaded shard is ready.
    return BindStatus::ready;
}

/** @return The bubble owning an effective region's one state; null for none or several. */
[[nodiscard]] const catalog::Bubble* owning_bubble(const GeneratedWorldView& view,
                                                   std::int32_t effectiveRegion) noexcept {
    const catalog::Snapshot* const snapshot = view.snapshot();
    if (snapshot == nullptr || effectiveRegion < 0) {
        return nullptr;
    }
    const catalog::State* selected = nullptr;
    for (const catalog::State& state : snapshot->states) {
        const std::uint64_t region = static_cast<std::uint64_t>(state.sliceSetIndex) + state.index;
        if (region != static_cast<std::uint32_t>(effectiveRegion)) {
            continue;
        }
        if (selected != nullptr) {
            return nullptr;
        }
        selected = &state;
    }
    if (selected == nullptr || selected->bubbleRow >= snapshot->bubbles.size()) {
        return nullptr;
    }
    const catalog::Bubble& bubble = snapshot->bubbles[selected->bubbleRow];
    if (bubble.index != selected->bubbleRow || selected->index >= bubble.stateCount
        || bubble.firstState > snapshot->states.size()
        || selected->index >= snapshot->states.size() - bubble.firstState
        || &snapshot->states[bubble.firstState + selected->index] != selected) {
        return nullptr;
    }
    return &bubble;
}

} // namespace

/** @return The stable log name of one bind status. */
const char* status_name(BindStatus value) noexcept {
    switch (value) {
    case BindStatus::ready:
        return "ready";
    case BindStatus::invalidBoundView:
        return "invalid_bound_view";
    case BindStatus::contentManifestUnavailable:
        return "content_manifest_unavailable";
    case BindStatus::pathUnavailable:
        return "path_unavailable";
    case BindStatus::manifestMissing:
        return "manifest_missing";
    case BindStatus::manifestStale:
        return "manifest_stale";
    case BindStatus::manifestSdkMismatch:
        return "manifest_sdk_mismatch";
    case BindStatus::manifestInvalid:
        return "manifest_invalid";
    case BindStatus::scenarioMissing:
        return "scenario_missing";
    case BindStatus::scenarioMismatch:
        return "scenario_mismatch";
    case BindStatus::shardMissing:
        return "shard_missing";
    case BindStatus::shardStale:
        return "shard_stale";
    case BindStatus::shardInvalid:
        return "shard_invalid";
    }
    return "shard_invalid";
}

/** Reads one effective region's package-authored public-bubble flag. */
bool region_is_public(const GeneratedWorldView& view,
                      std::int32_t effectiveRegion,
                      bool& isPublic) noexcept {
    const catalog::Bubble* const bubble = owning_bubble(view, effectiveRegion);
    isPublic = bubble != nullptr && bubble->isPublic;
    return bubble != nullptr;
}

/** Reads the name hash of the bubble one effective region belongs to. */
bool region_bubble_name(const GeneratedWorldView& view,
                        std::int32_t effectiveRegion,
                        std::uint32_t& nameHash) noexcept {
    const catalog::Bubble* const bubble = owning_bubble(view, effectiveRegion);
    nameHash = bubble != nullptr ? bubble->nameHash : 0;
    return nameHash != 0;
}

/** Reads the public flag through the shard, or a mod pack's own state rows. */
bool region_is_public(const activity_sdk::BoundView& activitySdkView,
                      std::int32_t effectiveRegion,
                      bool& isPublic) noexcept {
    isPublic = false;
    if (activitySdkView.catalog != nullptr && activitySdkView.catalog->from_mod()) {
        return activity_sdk::mod_region_is_public(activitySdkView, effectiveRegion, isPublic);
    }
    GeneratedWorldView world{};
    return resolve(activitySdkView, world) == BindStatus::ready
           && region_is_public(world, effectiveRegion, isPublic);
}

/** A bubble whose flag does not read counts as not public. */
bool public_bubble_mask(const activity_sdk::BoundView& activitySdkView,
                        std::size_t bubbleCount,
                        std::int32_t regionsPerBubble,
                        std::uint64_t& mask) noexcept {
    mask = 0;
    const bool mod = activitySdkView.catalog != nullptr && activitySdkView.catalog->from_mod();
    GeneratedWorldView world{};
    if (!mod && resolve(activitySdkView, world) != BindStatus::ready) {
        return false;
    }
    const std::size_t count =
        (std::min)(bubbleCount, std::size_t{std::numeric_limits<std::uint64_t>::digits});
    for (std::size_t bubble = 0; bubble < count; ++bubble) {
        const auto region = static_cast<std::int32_t>(bubble) * regionsPerBubble;
        bool isPublic = false;
        const bool known =
            mod ? activity_sdk::mod_region_is_public(activitySdkView, region, isPublic)
                : region_is_public(world, region, isPublic);
        if (known && isPublic) {
            mask |= std::uint64_t{1} << bubble;
        }
    }
    return true;
}

/** Binds the generated world that matches one activity SDK view. */
BindStatus resolve(const activity_sdk::BoundView& activitySdkView,
                   GeneratedWorldView& output) noexcept {
    output = {};
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
    (void)activitySdkView;
    return BindStatus::contentManifestUnavailable;
#else
    if (activitySdkView.catalog == nullptr
        || activitySdkView.catalog->artifact_directory().empty()) {
        return BindStatus::invalidBoundView;
    }
    Digest sourceFingerprint{};
    if (!state::content_manifest::installed_fingerprint(sourceFingerprint)
        || sourceFingerprint == Digest{}) {
        return BindStatus::contentManifestUnavailable;
    }
    core::path::Buffer manifestPath;
    core::path::Buffer scenarioDirectory;
    if (!joined_path(activitySdkView.catalog->artifact_directory(), kManifestSuffix, manifestPath)
        || !joined_path(activitySdkView.catalog->artifact_directory(),
                        kScenarioDirectorySuffix,
                        scenarioDirectory)) {
        return BindStatus::pathUnavailable;
    }
    Resolved resolved{};
    const BindStatus status = resolve_paths(activitySdkView,
                                            manifestPath.chars.data(),
                                            scenarioDirectory.chars.data(),
                                            sourceFingerprint,
                                            resolved);
    if (status == BindStatus::ready) {
        output = GeneratedWorldView(activitySdkView,
                                    resolved.scenarioTag,
                                    resolved.scenarioName,
                                    resolved.sourceFingerprint,
                                    resolved.manifestPayloadSha256,
                                    resolved.shardPayloadSha256,
                                    std::move(resolved.manifestCatalog),
                                    std::move(resolved.snapshot));
    }
    return status;
#endif
}

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Test-only bind that takes its manifest and scenario paths directly. */
BindStatus resolve_paths_for_test(const activity_sdk::BoundView& activitySdkView,
                                  const wchar_t* manifestPath,
                                  const wchar_t* scenarioDirectory,
                                  const Digest& sourceFingerprint,
                                  GeneratedWorldView& output) noexcept {
    output = {};
    Resolved resolved{};
    const BindStatus status = resolve_paths(
        activitySdkView, manifestPath, scenarioDirectory, sourceFingerprint, resolved);
    if (status == BindStatus::ready) {
        output = GeneratedWorldView(activitySdkView,
                                    resolved.scenarioTag,
                                    resolved.scenarioName,
                                    resolved.sourceFingerprint,
                                    resolved.manifestPayloadSha256,
                                    resolved.shardPayloadSha256,
                                    std::move(resolved.manifestCatalog),
                                    std::move(resolved.snapshot));
    }
    return status;
}

void clear_cache_for_test() noexcept {
    AcquireSRWLockExclusive(&g_cacheLock);
    g_cache = {};
    g_nextCacheEntry = 0;
    ReleaseSRWLockExclusive(&g_cacheLock);
}
#endif

} // namespace sunrise::state::activity_sdk::generated_world
