#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "../runtime.h"
#include "store.h"

namespace sunrise::state::activity_sdk::generated_world {

/** Exact outcomes from binding a generated-world shard to one canonical activity view. */
enum class BindStatus : std::uint8_t {
    ready,
    invalidBoundView,
    contentManifestUnavailable,
    pathUnavailable,
    manifestMissing,
    manifestStale,
    manifestSdkMismatch,
    manifestInvalid,
    scenarioMissing,
    scenarioMismatch,
    shardMissing,
    shardStale,
    shardInvalid,
};

/** Every digest that must change the generation of a world-data-backed program. */
struct GenerationIdentity final {
    Digest sdkBuildSha256{};
    Digest sdkPayloadSha256{};
    Digest sourceFingerprint{};
    Digest manifestPayloadSha256{};
    Digest shardPayloadSha256{};

    [[nodiscard]] bool operator==(const GenerationIdentity&) const noexcept = default;
};

/** One exact SDK binding plus its immutable generated-world scenario snapshot. */
class GeneratedWorldView final {
public:
    GeneratedWorldView() noexcept = default;

    /** @return The canonical activity SDK view that selected this scenario. */
    [[nodiscard]] const activity_sdk::BoundView& activity_sdk_view() const noexcept;
    /** @return The exact scenario package tag retained by both SDK artifacts. */
    [[nodiscard]] std::uint32_t scenario_tag() const noexcept;
    /** @return The exact canonical scenario name retained by both SDK artifacts. */
    [[nodiscard]] std::string_view scenario_name() const noexcept;
    /** @return The live installed-content fingerprint used to authenticate both files. */
    [[nodiscard]] std::span<const std::byte, 32> source_fingerprint() const noexcept;
    /** @return The exact canonical SDK-pack payload digest retained by this generation. */
    [[nodiscard]] std::span<const std::byte, 32> sdk_payload_sha256() const noexcept;
    /** @return The authenticated payload digest of the manifest that selected this shard. */
    [[nodiscard]] std::span<const std::byte, 32> manifest_payload_sha256() const noexcept;
    /** @return The payload digest reproduced by the authenticated shard loader. */
    [[nodiscard]] std::span<const std::byte, 32> shard_payload_sha256() const noexcept;
    /** @return The complete authenticated manifest that selected this scenario generation. */
    [[nodiscard]] const manifest::Catalog* manifest_catalog() const noexcept;
    /** @return The full tuple that owns one world-data-backed program generation. */
    [[nodiscard]] const GenerationIdentity& generation_identity() const noexcept;
    /** @return The extraction coverage authenticated inside the shard. */
    [[nodiscard]] build_data::scriptables::BuildCoverage coverage() const noexcept;
    /** @return The immutable generated-world snapshot, or null for an unbound view. */
    [[nodiscard]] const build_data::scriptables::Snapshot* snapshot() const noexcept;

private:
    friend BindStatus resolve(const activity_sdk::BoundView&, GeneratedWorldView&) noexcept;
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
    friend BindStatus resolve_paths_for_test(const activity_sdk::BoundView&,
                                             const wchar_t*,
                                             const wchar_t*,
                                             const Digest&,
                                             GeneratedWorldView&) noexcept;
#endif

    GeneratedWorldView(const activity_sdk::BoundView& activitySdkView,
                       std::uint32_t scenarioTag,
                       std::string_view scenarioName,
                       const Digest& sourceFingerprint,
                       const Digest& manifestPayloadSha256,
                       const Digest& shardPayloadSha256,
                       std::shared_ptr<const manifest::Catalog> manifestCatalog,
                       std::shared_ptr<const build_data::scriptables::Snapshot> snapshot) noexcept;

    activity_sdk::BoundView activitySdkView_{};
    std::uint32_t scenarioTag_{};
    std::array<char, build_data::scriptables::kScenarioNameCapacity> scenarioName_{};
    std::uint8_t scenarioNameLength_{};
    GenerationIdentity generationIdentity_{};
    build_data::scriptables::BuildCoverage coverage_{build_data::scriptables::BuildCoverage::none};
    std::shared_ptr<const manifest::Catalog> manifestCatalog_{};
    std::shared_ptr<const build_data::scriptables::Snapshot> snapshot_{};
};

/** Copies the whole bound identity so the view stays valid for the snapshot's lifetime. */
inline GeneratedWorldView::GeneratedWorldView(
    const activity_sdk::BoundView& activitySdkView,
    std::uint32_t scenarioTag,
    std::string_view scenarioName,
    const Digest& sourceFingerprint,
    const Digest& manifestPayloadSha256,
    const Digest& shardPayloadSha256,
    std::shared_ptr<const manifest::Catalog> manifestCatalog,
    std::shared_ptr<const build_data::scriptables::Snapshot> snapshot) noexcept
    : activitySdkView_(activitySdkView), scenarioTag_(scenarioTag),
      scenarioNameLength_(static_cast<std::uint8_t>(scenarioName.size())),
      coverage_(snapshot != nullptr ? snapshot->coverage
                                    : build_data::scriptables::BuildCoverage::none),
      manifestCatalog_(std::move(manifestCatalog)), snapshot_(std::move(snapshot)) {
    std::copy(scenarioName.begin(), scenarioName.end(), scenarioName_.begin());
    const auto sdkBuild = activitySdkView.catalog != nullptr
                              ? activitySdkView.catalog->sdk_build_sha256()
                              : std::span<const std::byte>{};
    const auto sdkPayload = activitySdkView.catalog != nullptr
                                ? activitySdkView.catalog->payload_sha256()
                                : std::span<const std::byte>{};
    if (sdkBuild.size() == generationIdentity_.sdkBuildSha256.size()) {
        std::copy(sdkBuild.begin(), sdkBuild.end(), generationIdentity_.sdkBuildSha256.begin());
    }
    if (sdkPayload.size() == generationIdentity_.sdkPayloadSha256.size()) {
        std::copy(
            sdkPayload.begin(), sdkPayload.end(), generationIdentity_.sdkPayloadSha256.begin());
    }
    generationIdentity_.sourceFingerprint = sourceFingerprint;
    generationIdentity_.manifestPayloadSha256 = manifestPayloadSha256;
    generationIdentity_.shardPayloadSha256 = shardPayloadSha256;
}

inline const activity_sdk::BoundView& GeneratedWorldView::activity_sdk_view() const noexcept {
    return activitySdkView_;
}

inline std::uint32_t GeneratedWorldView::scenario_tag() const noexcept {
    return scenarioTag_;
}

inline std::string_view GeneratedWorldView::scenario_name() const noexcept {
    return {scenarioName_.data(), scenarioNameLength_};
}

inline std::span<const std::byte, 32> GeneratedWorldView::source_fingerprint() const noexcept {
    return generationIdentity_.sourceFingerprint;
}

inline std::span<const std::byte, 32> GeneratedWorldView::sdk_payload_sha256() const noexcept {
    return generationIdentity_.sdkPayloadSha256;
}

inline std::span<const std::byte, 32> GeneratedWorldView::manifest_payload_sha256() const noexcept {
    return generationIdentity_.manifestPayloadSha256;
}

inline std::span<const std::byte, 32> GeneratedWorldView::shard_payload_sha256() const noexcept {
    return generationIdentity_.shardPayloadSha256;
}

inline const manifest::Catalog* GeneratedWorldView::manifest_catalog() const noexcept {
    return manifestCatalog_.get();
}

inline const GenerationIdentity& GeneratedWorldView::generation_identity() const noexcept {
    return generationIdentity_;
}

inline build_data::scriptables::BuildCoverage GeneratedWorldView::coverage() const noexcept {
    return coverage_;
}

inline const build_data::scriptables::Snapshot* GeneratedWorldView::snapshot() const noexcept {
    return coverage_ == build_data::scriptables::BuildCoverage::full ? snapshot_.get() : nullptr;
}

/** @return Stable machine-readable text for one generated-world binding result. */
[[nodiscard]] const char* status_name(BindStatus value) noexcept;

/**
 * Reads the package-authored visibility of one effective region.
 * @param view Exact generated-world binding for the selected activity.
 * @param effectiveRegion Package slice-set index plus state ordinal.
 * @param isPublic Cleared, then receives the owning bubble's extracted flag.
 * @return True when exactly one state owns the effective region.
 */
[[nodiscard]] bool region_is_public(const GeneratedWorldView& view,
                                    std::int32_t effectiveRegion,
                                    bool& isPublic) noexcept;

/**
 * Reads the name of the bubble one effective region belongs to.
 * A bubble's name is unique only inside its scenario.
 * @param view Exact generated-world binding for the selected activity.
 * @param effectiveRegion Package slice-set index plus state ordinal.
 * @param nameHash Receives the owning bubble's name hash, or zero.
 * @return True when exactly one state owns the effective region and its bubble is named.
 */
[[nodiscard]] bool region_bubble_name(const GeneratedWorldView& view,
                                      std::int32_t effectiveRegion,
                                      std::uint32_t& nameHash) noexcept;

/** Resolves the exact live-content shard selected by one canonical activity SDK view. */
[[nodiscard]] BindStatus resolve(const activity_sdk::BoundView& activitySdkView,
                                 GeneratedWorldView& output) noexcept;

/**
 * Reads one effective region's public flag for a bound activity: from the shard, or from the pack's
 * state rows when a mod ships the activity, since mods have no shard.
 * @return True when exactly one state owns the effective region.
 */
[[nodiscard]] bool region_is_public(const activity_sdk::BoundView& activitySdkView,
                                    std::int32_t effectiveRegion,
                                    bool& isPublic) noexcept;

/**
 * Sets bit b of `mask` for each public bubble b, read at the bubble's first effective region.
 * The shard is bound once for every bubble; a mod activity reads its pack's state rows.
 * @param bubbleCount Bubbles to read, at most 64.
 * @param regionsPerBubble Effective regions one bubble spans.
 * @return False when the activity's shard does not bind.
 */
[[nodiscard]] bool public_bubble_mask(const activity_sdk::BoundView& activitySdkView,
                                      std::size_t bubbleCount,
                                      std::int32_t regionsPerBubble,
                                      std::uint64_t& mask) noexcept;

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Resolves explicit artifact paths and a source pin in focused offline tests. */
[[nodiscard]] BindStatus resolve_paths_for_test(const activity_sdk::BoundView& activitySdkView,
                                                const wchar_t* manifestPath,
                                                const wchar_t* scenarioDirectory,
                                                const Digest& sourceFingerprint,
                                                GeneratedWorldView& output) noexcept;
/** Drops only the bounded decoded-shard cache used by focused offline tests. */
void clear_cache_for_test() noexcept;
#endif

} // namespace sunrise::state::activity_sdk::generated_world
