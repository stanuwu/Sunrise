#pragma once

#include <Windows.h>

#include <array>
#include <bcrypt.h>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../definition.h"

namespace sunrise::state::content_manifest::fingerprint {

/** Fixed CNG object storage is larger than the SHA-256 provider asks for. */
inline constexpr std::size_t kHashObjectCapacity = 512;

/** Fixed-storage Windows CNG SHA-256 operation. */
class Hasher final {
public:
    /** Opens one SHA-256 provider and creates a hash over fixed caller-owned object storage. */
    Hasher() noexcept;
    /** Releases CNG handles and clears provider object bytes. */
    ~Hasher() noexcept;

    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    /** @return True when the provider and hash operation are ready. */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * Extends the pending SHA-256 digest with exact bytes.
     * @param bytes Public canonical identity bytes.
     * @return True when CNG consumes the complete input.
     */
    [[nodiscard]] bool update(std::span<const std::byte> bytes) noexcept;

    /**
     * Finishes the digest and releases the one-use hash operation.
     * @param output Cleared SHA-256 result.
     * @return True when CNG writes all 32 bytes.
     */
    [[nodiscard]] bool finish(Fingerprint& output) noexcept;

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
    std::array<std::byte, kHashObjectCapacity> object_{};
};

/** @param hasher Pending digest. @param value Canonical integer. @return CNG update result. */
[[nodiscard]] bool update_u16(Hasher& hasher, std::uint16_t value) noexcept;
/** @param hasher Pending digest. @param value Canonical integer. @return CNG update result. */
[[nodiscard]] bool update_u32(Hasher& hasher, std::uint32_t value) noexcept;
/** @param hasher Pending digest. @param value Canonical integer. @return CNG update result. */
[[nodiscard]] bool update_u64(Hasher& hasher, std::uint64_t value) noexcept;

/**
 * Builds the public fingerprint for a complete canonical row set.
 * @param rows Sorted, already-checked rows.
 * @param output Cleared SHA-256 result.
 * @return True when every canonical field is hashed.
 */
[[nodiscard]] bool rows(std::span<const Row> rows, Fingerprint& output) noexcept;

/**
 * Formats an application-defined UUID from a public row fingerprint.
 * @param buildFingerprint SHA-256 identity for the complete row set.
 * @param output Cleared lowercase canonical UUID text.
 */
void guid(const Fingerprint& buildFingerprint, Guid& output) noexcept;

} // namespace sunrise::state::content_manifest::fingerprint
