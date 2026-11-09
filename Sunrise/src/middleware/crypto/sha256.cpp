#include "sha256.h"

#include <Windows.h>

#include <bcrypt.h>
#include <limits>

namespace sunrise::middleware::crypto::sha256 {

namespace {

/** @return True for a BCrypt status that reports success. */
[[nodiscard]] bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

/**
 * Adds one buffer to an open hash.
 * @param handle Open hash object.
 * @param part Bytes to add; an empty part is skipped.
 * @return True when the bytes fit one call and BCrypt accepted them.
 */
[[nodiscard]] bool add(BCRYPT_HASH_HANDLE handle, std::span<const std::byte> part) noexcept {
    if (part.empty()) {
        return true;
    }
    if (part.size() > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    return succeeded(BCryptHashData(handle,
                                    reinterpret_cast<PUCHAR>(const_cast<std::byte*>(part.data())),
                                    static_cast<ULONG>(part.size()),
                                    0));
}

/**
 * Hashes up to two buffers as one message.
 * @param first Leading bytes.
 * @param second Trailing bytes, possibly empty.
 * @param output Receives the digest only on success.
 * @return True when every stage of the Windows hash succeeded.
 */
[[nodiscard]] bool digest(std::span<const std::byte> first,
                          std::span<const std::byte> second,
                          Digest& output) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!succeeded(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    BCRYPT_HASH_HANDLE handle = nullptr;
    bool complete = false;
    if (succeeded(BCryptCreateHash(algorithm, &handle, nullptr, 0, nullptr, 0, 0))) {
        complete = add(handle, first) && add(handle, second)
                   && succeeded(BCryptFinishHash(handle,
                                                 reinterpret_cast<PUCHAR>(output.data()),
                                                 static_cast<ULONG>(output.size()),
                                                 0));
        BCryptDestroyHash(handle);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return complete;
}

} // namespace

/** Hashes one buffer. */
bool hash(std::span<const std::byte> input, Digest& output) noexcept {
    return digest(input, {}, output);
}

Stream::Stream() noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE handle = nullptr;
    ok_ = succeeded(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
          && succeeded(BCryptCreateHash(algorithm, &handle, nullptr, 0, nullptr, 0, 0));
    algorithm_ = algorithm;
    handle_ = handle;
}

Stream::~Stream() noexcept {
    if (handle_ != nullptr) {
        BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle_));
    }
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
    }
}

void Stream::add(std::span<const std::byte> part) noexcept {
    ok_ = ok_ && sha256::add(static_cast<BCRYPT_HASH_HANDLE>(handle_), part);
}

bool Stream::finish(Digest& output) noexcept {
    const bool done = ok_
                      && succeeded(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(handle_),
                                                    reinterpret_cast<PUCHAR>(output.data()),
                                                    static_cast<ULONG>(output.size()),
                                                    0));
    ok_ = false;
    return done;
}

/** Hashes two buffers as one concatenated message. */
bool hash_pair(std::span<const std::byte> first,
               std::span<const std::byte> second,
               Digest& output) noexcept {
    return digest(first, second, output);
}

} // namespace sunrise::middleware::crypto::sha256
