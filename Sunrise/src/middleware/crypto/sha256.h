#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sunrise::middleware::crypto::sha256 {

/** SHA-256 produces 32 bytes. */
inline constexpr std::size_t kDigestSize = 32;

/** One finished SHA-256 digest. */
using Digest = std::array<std::byte, kDigestSize>;

/**
 * Hashes one buffer.
 * @param input Bytes to hash.
 * @param output Receives the digest only on success.
 * @return True when Windows completed the hash.
 */
[[nodiscard]] bool hash(std::span<const std::byte> input, Digest& output) noexcept;

/**
 * Hashes two buffers as one concatenated message.
 * The SRP scrambling parameter is defined over a concatenation, so it needs no scratch copy.
 * @param first Leading bytes.
 * @param second Trailing bytes.
 * @param output Receives the digest only on success.
 * @return True when Windows completed the hash.
 */
[[nodiscard]] bool hash_pair(std::span<const std::byte> first,
                             std::span<const std::byte> second,
                             Digest& output) noexcept;

/** One SHA-256 fed in parts. Any failed step makes `finish` fail. */
class Stream final {
public:
    Stream() noexcept;
    ~Stream() noexcept;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /** Adds `part` to the message. */
    void add(std::span<const std::byte> part) noexcept;
    /** @return True and the digest when every step succeeded; the stream is then spent. */
    [[nodiscard]] bool finish(Digest& output) noexcept;

private:
    void* algorithm_{};
    void* handle_{};
    bool ok_{};
};

} // namespace sunrise::middleware::crypto::sha256
