#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../definition.h"
#include "package/content_manifest_package_name.h"

namespace sunrise::state::content_manifest::scanner {

/** One recognized file, kept only while the inventory and header checks run. */
struct Candidate final {
    package::ParsedName parsed;
    std::uint64_t fileSize{};
    std::uint64_t lastWriteTime{};
    std::uint64_t buildSignature{};
};

/**
 * Lists recognized package names and builds their public directory fingerprint.
 * @param directory Installed packages directory.
 * @param candidates Fixed scratch storage for every recognized file.
 * @param count Receives the candidate count.
 * @param fingerprint Receives the SHA-256 directory identity.
 * @return True when a nonempty, canonical inventory fits caller storage.
 */
[[nodiscard]] bool inventory(std::wstring_view directory,
                             std::span<Candidate> candidates,
                             std::size_t& count,
                             Fingerprint& fingerprint) noexcept;

/**
 * Checks every listed header and emits one row per installed file.
 * @param directory Installed packages directory used by inventory.
 * @param candidates Mutable inventory scratch, reordered during grouping.
 * @param rows Fixed output storage for canonical manifest rows.
 * @param count Receives the emitted row count.
 * @param buildFingerprint Receives the SHA-256 fingerprint of the emitted public rows.
 * @param guid Receives fixed UUID text built from the row fingerprint.
 * @return True when every file passes and each package id has one identity.
 */
[[nodiscard]] bool extract(std::wstring_view directory,
                           std::span<Candidate> candidates,
                           std::span<Row> rows,
                           std::size_t& count,
                           Fingerprint& buildFingerprint,
                           Guid& guid) noexcept;

} // namespace sunrise::state::content_manifest::scanner
