#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::state::content_manifest {

/** The client ContentConfig array accepts at most 4000 package rows. */
inline constexpr std::size_t kRowCapacity = 4000;
/** The client package-name field reserves 256 bytes, including its null. */
inline constexpr std::size_t kPackageNameCapacity = 256;
/** SHA-256 fingerprints hold 32 public identity bytes. */
inline constexpr std::size_t kFingerprintSize = 32;
/** Canonical UUID text holds 36 characters and one null. */
inline constexpr std::size_t kGuidCapacity = 37;
/** The package registrar accepts public package ids from 0x0100 through 0x19FF. */
inline constexpr std::uint16_t kMinimumPackageId = 0x0100;
/** The package registrar rejects public package ids above 0x19FF. */
inline constexpr std::uint16_t kMaximumPackageId = 0x19FF;

using Fingerprint = std::array<std::byte, kFingerprintSize>;
using Guid = std::array<char, kGuidCapacity>;

/** One public installed-package row kept by State. */
struct Row final {
    std::array<char, kPackageNameCapacity> name{};
    std::uint16_t nameLength{};
    std::uint16_t packageId{};
    std::uint64_t buildSignature{};
};

/** Immutable State view, valid only during one snapshot visit. */
struct View final {
    std::span<const Row> rows;
    std::string_view guid;
    std::span<const std::byte, kFingerprintSize> buildFingerprint;
};

/**
 * Visits one immutable content-manifest view while State holds its shared lock.
 * @param context Caller context passed through without ownership.
 * @param view Borrowed rows, UUID, and public build fingerprint.
 * @return True when the caller consumes the complete view.
 */
using SnapshotVisitor = bool (*)(void* context, const View& view) noexcept;

} // namespace sunrise::state::content_manifest
