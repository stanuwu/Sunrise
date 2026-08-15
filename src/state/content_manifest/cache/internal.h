#pragma once

#include <cstddef>
#include <span>

#include "../definition.h"

namespace sunrise::state::content_manifest::cache {

/** How one cache lookup compares with the current package directory. */
enum class LoadStatus : std::uint8_t {
    missing,
    stale,
    invalid,
    loaded,
};

/**
 * Loads and checks one exact cache into fixed caller storage.
 * @param path Null-terminated cache path.
 * @param directoryFingerprint Current recognized-file identity.
 * @param rows Fixed output storage, cleared on failure.
 * @param count Receives the loaded row count.
 * @param buildFingerprint Receives the selected-row fingerprint.
 * @param guid Receives a fixed UUID built from the selected rows.
 * @return Missing, stale, invalid, or fully loaded status.
 */
[[nodiscard]] LoadStatus load(const wchar_t* path,
                              const Fingerprint& directoryFingerprint,
                              std::span<Row> rows,
                              std::size_t& count,
                              Fingerprint& buildFingerprint,
                              Guid& guid) noexcept;

/**
 * Writes, reopens, checks, then publishes one generated cache with one rename.
 * @param directory Null-terminated owned cache directory.
 * @param path Null-terminated final cache path.
 * @param directoryFingerprint Current recognized-file identity.
 * @param buildFingerprint Selected-row fingerprint.
 * @param rows Complete canonical selected rows.
 * @param validationRows Fixed scratch used to reopen the temporary file.
 * @return True when the final path receives the complete checked cache.
 */
[[nodiscard]] bool write(const wchar_t* directory,
                         const wchar_t* path,
                         const Fingerprint& directoryFingerprint,
                         const Fingerprint& buildFingerprint,
                         std::span<const Row> rows,
                         std::span<Row> validationRows) noexcept;

} // namespace sunrise::state::content_manifest::cache
