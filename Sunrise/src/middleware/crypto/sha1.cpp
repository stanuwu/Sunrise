#include "sha1.h"

#include <Windows.h>

#include <bcrypt.h>
#include <limits>

namespace sunrise::middleware::crypto::sha1 {
namespace {

[[nodiscard]] bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

} // namespace

/** Hashes one buffer with Windows CNG. */
bool hash(std::span<const std::byte> input, Digest& output) noexcept {
    output = {};
    if (input.empty() || input.size() > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!succeeded(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) {
        return false;
    }
    BCRYPT_HASH_HANDLE handle = nullptr;
    bool complete = false;
    if (succeeded(BCryptCreateHash(algorithm, &handle, nullptr, 0, nullptr, 0, 0))) {
        complete =
            succeeded(BCryptHashData(handle,
                                     reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data())),
                                     static_cast<ULONG>(input.size()),
                                     0))
            && succeeded(BCryptFinishHash(handle,
                                          reinterpret_cast<PUCHAR>(output.data()),
                                          static_cast<ULONG>(output.size()),
                                          0));
        BCryptDestroyHash(handle);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return complete;
}

} // namespace sunrise::middleware::crypto::sha1
