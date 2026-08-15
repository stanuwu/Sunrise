#include "content_manifest_state_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/ui/busy/busy.h"
#include "cache/internal.h"
#include "scanner/internal.h"

namespace sunrise::state::content_manifest {
namespace {

/** Generated package manifests share the owned cache artifact directory. */
constexpr std::wstring_view kCacheDirectorySuffix = L"\\cache";
/** One reusable versioned file stores the public package manifest rows. */
constexpr std::wstring_view kCacheFileSuffix = L"\\cache\\content_manifest.bin";

/** State-owned immutable catalog and the lock that guards publishing. */
struct Storage final {
    SRWLOCK lock{SRWLOCK_INIT};
    std::array<Row, kRowCapacity> rows{};
    std::size_t rowCount{};
    Fingerprint buildFingerprint{};
    Guid guid{};
    bool ready{};
};

Storage g_storage;
std::array<scanner::Candidate, kRowCapacity> g_candidates{};
std::array<Row, kRowCapacity> g_stagedRows{};
std::array<Row, kRowCapacity> g_validationRows{};

/** Clears published fields, leaving the initialized Windows lock alone. */
void clear_storage() noexcept {
    SecureZeroMemory(g_storage.rows.data(), sizeof g_storage.rows);
    g_storage.rowCount = 0;
    g_storage.buildFingerprint = {};
    g_storage.guid = {};
    g_storage.ready = false;
}

/** Clears every fixed scratch buffer used by first boot and cache validation. */
void clear_scratch() noexcept {
    SecureZeroMemory(g_candidates.data(), sizeof g_candidates);
    SecureZeroMemory(g_stagedRows.data(), sizeof g_stagedRows);
    SecureZeroMemory(g_validationRows.data(), sizeof g_validationRows);
}

/**
 * Builds the two generated-cache paths under the module-relative artifact root.
 * @param module Loaded Sunrise module.
 * @param directory Receives the null-terminated cache directory.
 * @param path Receives the null-terminated final cache file path.
 * @return True when both complete paths fit fixed storage.
 */
[[nodiscard]] bool
cache_paths(void* module, core::path::Buffer& directory, core::path::Buffer& path) noexcept {
    core::path::Buffer artifact;
    if (!core::path::artifact_directory(module, artifact)) {
        return false;
    }
    directory = artifact;
    path = artifact;
    return core::path::append(directory, kCacheDirectorySuffix)
           && core::path::append(path, kCacheFileSuffix);
}

/**
 * Publishes one fully checked staged catalog into State in one step.
 * @param rows Canonical staged rows.
 * @param buildFingerprint Public selected-build identity.
 * @param guid Fixed UUID text built from the selected rows.
 */
void publish(std::span<const Row> rows,
             const Fingerprint& buildFingerprint,
             const Guid& guid) noexcept {
    clear_storage();
    std::copy(rows.begin(), rows.end(), g_storage.rows.begin());
    g_storage.rowCount = rows.size();
    g_storage.buildFingerprint = buildFingerprint;
    g_storage.guid = guid;
    g_storage.ready = true;
}

} // namespace

/** Loads a current cache or extracts one complete installed-package manifest. */
bool initialize(void* module, std::wstring_view packagesDirectory) noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    clear_storage();
    clear_scratch();

    std::size_t candidateCount = 0;
    Fingerprint directoryFingerprint{};
    bool complete =
        scanner::inventory(packagesDirectory, g_candidates, candidateCount, directoryFingerprint);
    core::path::Buffer cacheDirectory;
    core::path::Buffer cachePath;
    if (complete && module != nullptr) {
        complete = cache_paths(module, cacheDirectory, cachePath);
    }

    std::size_t rowCount = 0;
    Fingerprint buildFingerprint{};
    Guid guid{};
    cache::LoadStatus status = cache::LoadStatus::missing;
    if (complete && module != nullptr) {
        status = cache::load(cachePath.chars.data(),
                             directoryFingerprint,
                             g_stagedRows,
                             rowCount,
                             buildFingerprint,
                             guid);
    }
    if (complete && status != cache::LoadStatus::loaded) {
        // Only the read and the write that follows it stall. A cache that loaded skips both.
        core::ui::busy::begin(core::ui::busy::Task::manifestBuild);
        complete = scanner::extract(packagesDirectory,
                                    std::span(g_candidates).first(candidateCount),
                                    g_stagedRows,
                                    rowCount,
                                    buildFingerprint,
                                    guid);
        if (complete && module != nullptr) {
            // Missing, stale and invalid files all take the same checked replacement path.
            complete = cache::write(cacheDirectory.chars.data(),
                                    cachePath.chars.data(),
                                    directoryFingerprint,
                                    buildFingerprint,
                                    std::span(g_stagedRows).first(rowCount),
                                    g_validationRows);
        }
        core::ui::busy::end(core::ui::busy::Task::manifestBuild);
    }
    if (complete) {
        publish(std::span(g_stagedRows).first(rowCount), buildFingerprint, guid);
    }
    clear_scratch();
    ReleaseSRWLockExclusive(&g_storage.lock);
    return complete;
}

/** Clears the generated manifest, fingerprints, GUID, and extraction scratch. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_storage.lock);
    clear_storage();
    clear_scratch();
    ReleaseSRWLockExclusive(&g_storage.lock);
}

/** Visits one immutable catalog without leaking State-owned pointers after the call. */
bool visit_snapshot(SnapshotVisitor visitor, void* context) noexcept {
    if (visitor == nullptr) {
        return false;
    }
    AcquireSRWLockShared(&g_storage.lock);
    if (!g_storage.ready) {
        ReleaseSRWLockShared(&g_storage.lock);
        return false;
    }
    const View view{
        std::span(g_storage.rows).first(g_storage.rowCount),
        std::string_view(g_storage.guid.data(), kGuidCapacity - 1),
        std::span<const std::byte, kFingerprintSize>(g_storage.buildFingerprint),
    };
    // The view cannot escape because State releases the read lock after this callback returns.
    const bool result = visitor(context, view);
    ReleaseSRWLockShared(&g_storage.lock);
    return result;
}

} // namespace sunrise::state::content_manifest
