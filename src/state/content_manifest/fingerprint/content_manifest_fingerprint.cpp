#include "content_manifest_fingerprint.h"

#include <Windows.h>

#include <algorithm>
#include <limits>

namespace sunrise::state::content_manifest::fingerprint {
namespace {

/** A zero byte ends each fingerprint domain label, before its version. */
constexpr std::byte kDomainTerminator{0};
/** Row fingerprint domain version 1 owns the current canonical field order. */
constexpr std::byte kRowDomainVersion{1};
/** This domain separates row identities from other Sunrise SHA-256 uses. */
constexpr std::array<std::byte, 20> kRowDomain{
    std::byte{'S'}, std::byte{'u'}, std::byte{'n'}, std::byte{'r'},    std::byte{'i'},
    std::byte{'s'}, std::byte{'e'}, std::byte{'C'}, std::byte{'o'},    std::byte{'n'},
    std::byte{'t'}, std::byte{'e'}, std::byte{'n'}, std::byte{'t'},    std::byte{'R'},
    std::byte{'o'}, std::byte{'w'}, std::byte{'s'}, kDomainTerminator, kRowDomainVersion,
};
/** UUID version 8 reserves the payload bits for this deterministic public hash. */
constexpr std::uint8_t kUuidVersionBits = 0x80;
/** RFC UUID variants set the high 2 bits of byte 8 to binary 10. */
constexpr std::uint8_t kUuidVariantBits = 0x80;
/** 4 low bits stay free beside the UUID version nibble. */
constexpr std::uint8_t kUuidVersionPayloadMask = 0x0F;
/** 6 low bits stay free beside the RFC UUID variant bits. */
constexpr std::uint8_t kUuidVariantPayloadMask = 0x3F;
/** UUID byte 6 carries its 4-bit version. */
constexpr std::size_t kUuidVersionByte = 6;
/** UUID byte 8 carries its 2-bit variant. */
constexpr std::size_t kUuidVariantByte = 8;
/** Canonical UUID text places hyphens after byte groups 4, 6, 8, and 10. */
constexpr std::array<std::size_t, 4> kUuidHyphenOffsets{4, 6, 8, 10};
/** Lowercase hex keeps cache-derived GUID text canonical. */
constexpr std::string_view kHexDigits = "0123456789abcdef";

/** @param status Windows CNG status. @return True for a successful NTSTATUS. */
[[nodiscard]] bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

} // namespace

/** Opens one SHA-256 operation over fixed provider object storage. */
Hasher::Hasher() noexcept {
    if (!succeeded(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        algorithm_ = nullptr;
        return;
    }
    DWORD objectLength = 0;
    DWORD copied = 0;
    if (!succeeded(BCryptGetProperty(algorithm_,
                                     BCRYPT_OBJECT_LENGTH,
                                     reinterpret_cast<PUCHAR>(&objectLength),
                                     sizeof objectLength,
                                     &copied,
                                     0))
        || copied != sizeof objectLength || objectLength > object_.size()
        || !succeeded(BCryptCreateHash(algorithm_,
                                       &hash_,
                                       reinterpret_cast<PUCHAR>(object_.data()),
                                       objectLength,
                                       nullptr,
                                       0,
                                       0))) {
        BCryptCloseAlgorithmProvider(algorithm_, 0);
        algorithm_ = nullptr;
        hash_ = nullptr;
    }
}

/** Releases CNG handles and clears provider object bytes. */
Hasher::~Hasher() noexcept {
    if (hash_ != nullptr) {
        BCryptDestroyHash(hash_);
    }
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    SecureZeroMemory(object_.data(), object_.size());
}

/** @return True when the provider and hash operation are ready. */
bool Hasher::ready() const noexcept {
    return algorithm_ != nullptr && hash_ != nullptr;
}

/** Extends the pending SHA-256 digest with exact bytes. */
bool Hasher::update(std::span<const std::byte> bytes) noexcept {
    if (!ready()) {
        return false;
    }
    while (!bytes.empty()) {
        const std::size_t chunkSize =
            (std::min)(bytes.size(), static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()));
        if (!succeeded(
                BCryptHashData(hash_,
                               const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())),
                               static_cast<ULONG>(chunkSize),
                               0))) {
            return false;
        }
        bytes = bytes.subspan(chunkSize);
    }
    return true;
}

/** Finishes the digest and releases the one-use hash operation. */
bool Hasher::finish(Fingerprint& output) noexcept {
    output = {};
    if (!ready()
        || !succeeded(BCryptFinishHash(hash_,
                                       reinterpret_cast<PUCHAR>(output.data()),
                                       static_cast<ULONG>(output.size()),
                                       0))) {
        return false;
    }
    BCryptDestroyHash(hash_);
    hash_ = nullptr;
    return true;
}

/** @param hasher Pending digest. @param value Canonical integer. @return CNG update result. */
bool update_u16(Hasher& hasher, std::uint16_t value) noexcept {
    const std::array<std::byte, 2> encoded{
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
    };
    return hasher.update(encoded);
}

/** @param hasher Pending digest. @param value Canonical integer. @return CNG update result. */
bool update_u32(Hasher& hasher, std::uint32_t value) noexcept {
    std::array<std::byte, 4> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    return hasher.update(encoded);
}

/** @param hasher Pending digest. @param value Canonical integer. @return CNG update result. */
bool update_u64(Hasher& hasher, std::uint64_t value) noexcept {
    std::array<std::byte, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    return hasher.update(encoded);
}

/** Builds the public fingerprint for a complete canonical row set. */
bool rows(std::span<const Row> manifestRows, Fingerprint& output) noexcept {
    output = {};
    Hasher hasher;
    if (!hasher.update(kRowDomain)
        || !update_u32(hasher, static_cast<std::uint32_t>(manifestRows.size()))) {
        return false;
    }
    for (const Row& row : manifestRows) {
        const auto nameBytes =
            std::as_bytes(std::span(row.name.data(), static_cast<std::size_t>(row.nameLength)));
        if (!update_u16(hasher, row.nameLength) || !hasher.update(nameBytes)
            || !update_u16(hasher, row.packageId) || !update_u64(hasher, row.buildSignature)) {
            return false;
        }
    }
    return hasher.finish(output);
}

/** Formats an application-defined UUID from a public row fingerprint. */
void guid(const Fingerprint& buildFingerprint, Guid& output) noexcept {
    output = {};
    std::array<std::uint8_t, 16> uuid{};
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        uuid[index] = std::to_integer<std::uint8_t>(buildFingerprint[index]);
    }
    uuid[kUuidVersionByte] = static_cast<std::uint8_t>(
        (uuid[kUuidVersionByte] & kUuidVersionPayloadMask) | kUuidVersionBits);
    uuid[kUuidVariantByte] = static_cast<std::uint8_t>(
        (uuid[kUuidVariantByte] & kUuidVariantPayloadMask) | kUuidVariantBits);

    std::size_t cursor = 0;
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        if (std::find(kUuidHyphenOffsets.begin(), kUuidHyphenOffsets.end(), index)
            != kUuidHyphenOffsets.end()) {
            output[cursor++] = '-';
        }
        output[cursor++] = kHexDigits[uuid[index] >> 4U];
        output[cursor++] = kHexDigits[uuid[index] & 0x0FU];
    }
    output[cursor] = '\0';
}

} // namespace sunrise::state::content_manifest::fingerprint
