#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "../../middleware/crypto/sha256.h"

namespace sunrise::state::activity_sdk::identity {

/** One SHA-256 identity used by the runtime SDK trust chain. */
using Digest = std::array<std::byte, 32>;

/** Catalog-authorized identity required before a runtime pack can be mapped. */
struct Expected final {
    Digest sdkBuildSha256{};
    Digest payloadSha256{};
    Digest contentKeySha256{};
    Digest logicalIrSha256{};

    [[nodiscard]] bool operator==(const Expected&) const noexcept = default;
};

/** @return True when a digest is not the invalid all-zero sentinel. */
[[nodiscard]] inline bool valid(const Digest& value) noexcept {
    return value != Digest{};
}

/**
 * Derives every non-payload header identity from authenticated installed content and packed rows.
 * The payload digest is already the canonical serialized logical row projection; domain separation
 * prevents either derived digest from being mistaken for an untyped content hash.
 */
[[nodiscard]] inline bool
derive(const Digest& sourceFingerprint, const Digest& payloadSha256, Expected& output) noexcept {
    output = {};
    if (!valid(sourceFingerprint) || !valid(payloadSha256)) {
        return false;
    }
    constexpr auto kLogicalDomain = std::to_array("sunrise-activity-sdk-logical-ir-v1");
    // Regenerate packs whose squad associations used the single-rule admission check.
    constexpr auto kBuildDomain = std::to_array("sunrise-activity-sdk-build-v14");
    const auto logicalDomain = std::as_bytes(std::span(kLogicalDomain));
    const auto buildDomain = std::as_bytes(std::span(kBuildDomain));
    Digest logical{};
    if (!middleware::crypto::sha256::hash_pair(logicalDomain, payloadSha256, logical)
        || !valid(logical)) {
        return false;
    }
    std::array<std::byte, 64> buildMaterial{};
    std::copy(sourceFingerprint.begin(), sourceFingerprint.end(), buildMaterial.begin());
    std::copy(logical.begin(), logical.end(), buildMaterial.begin() + sourceFingerprint.size());
    Digest build{};
    if (!middleware::crypto::sha256::hash_pair(buildDomain, buildMaterial, build)
        || !valid(build)) {
        return false;
    }
    output = {build, payloadSha256, sourceFingerprint, logical};
    return true;
}

} // namespace sunrise::state::activity_sdk::identity
