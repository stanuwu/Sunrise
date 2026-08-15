#include <algorithm>
#include <string_view>

#include "../content_manifest_row_validation.h"
#include "../fingerprint/content_manifest_fingerprint.h"
#include "internal.h"
#include "package/content_manifest_package_header.h"

namespace sunrise::state::content_manifest::scanner {
namespace {

/** @param first Candidate. @param second Candidate. @return Package grouping order. */
[[nodiscard]] bool package_less(const Candidate& first, const Candidate& second) noexcept {
    if (first.parsed.packageId != second.parsed.packageId) {
        return first.parsed.packageId < second.parsed.packageId;
    }
    const std::string_view firstIdentity(first.parsed.stem.data(), first.parsed.identityLength);
    const std::string_view secondIdentity(second.parsed.stem.data(), second.parsed.identityLength);
    if (firstIdentity != secondIdentity) {
        return firstIdentity < secondIdentity;
    }
    return first.parsed.patchIndex < second.parsed.patchIndex;
}

/** @param first Candidate. @param second Candidate. @return Exact package identity match. */
[[nodiscard]] bool same_identity(const Candidate& first, const Candidate& second) noexcept {
    return first.parsed.packageId == second.parsed.packageId
           && first.parsed.identityLength == second.parsed.identityLength
           && std::equal(first.parsed.stem.begin(),
                         first.parsed.stem.begin() + first.parsed.identityLength,
                         second.parsed.stem.begin());
}

/**
 * Copies one checked candidate into canonical State row storage.
 * @param candidate One installed package file.
 * @param output Cleared destination row.
 */
void copy_row(const Candidate& candidate, Row& output) noexcept {
    output = {};
    const package::ParsedName& parsed = candidate.parsed;
    std::copy(parsed.stem.begin(), parsed.stem.begin() + parsed.stemLength, output.name.begin());
    output.nameLength = parsed.stemLength;
    output.packageId = parsed.packageId;
    output.buildSignature = candidate.buildSignature;
}

} // namespace

/** Checks every listed header and emits one row per installed file. */
bool extract(std::wstring_view directory,
             std::span<Candidate> candidates,
             std::span<Row> rows,
             std::size_t& count,
             Fingerprint& buildFingerprint,
             Guid& guidValue) noexcept {
    count = 0;
    buildFingerprint = {};
    guidValue = {};
    if (candidates.empty() || candidates.size() > kRowCapacity) {
        return false;
    }
    for (Candidate& candidate : candidates) {
        if (!package::read_header(directory, candidate)) {
            return false;
        }
    }

    std::sort(candidates.begin(), candidates.end(), package_less);
    std::size_t cursor = 0;
    while (cursor < candidates.size()) {
        const std::size_t groupStart = cursor;
        std::uint32_t previousPatch = candidates[cursor].parsed.patchIndex;
        do {
            // One public id cannot safely describe two different package identities.
            if (cursor != groupStart
                && (!same_identity(candidates[groupStart], candidates[cursor])
                    || candidates[cursor].parsed.patchIndex == previousPatch)) {
                return false;
            }
            if (count == rows.size()) {
                return false;
            }
            previousPatch = candidates[cursor].parsed.patchIndex;
            copy_row(candidates[cursor++], rows[count++]);
        } while (cursor < candidates.size()
                 && candidates[cursor].parsed.packageId == candidates[groupStart].parsed.packageId);
    }

    std::span<Row> occupied = rows.first(count);
    std::sort(occupied.begin(), occupied.end(), [](const Row& first, const Row& second) {
        return std::string_view(first.name.data(), first.nameLength)
               < std::string_view(second.name.data(), second.nameLength);
    });
    if (!valid(std::span<const Row>(occupied)) || !fingerprint::rows(occupied, buildFingerprint)) {
        count = 0;
        return false;
    }
    fingerprint::guid(buildFingerprint, guidValue);
    return true;
}

} // namespace sunrise::state::content_manifest::scanner
