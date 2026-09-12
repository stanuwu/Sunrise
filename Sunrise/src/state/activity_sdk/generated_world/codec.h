#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../build_data/scriptables/definition.h"

namespace sunrise::state::activity_sdk::generated_world {

/** One SHA-256 identity used for both source and payload pins. */
using Digest = std::array<std::byte, 32>;

/** Result of loading one generated-world scenario shard. */
enum class LoadStatus : std::uint8_t {
    loaded,
    missing,
    versionMismatch,
    scenarioMismatch,
    sourceMismatch,
    invalid,
};

/**
 * One complete shard image produced by `prepare` and consumed by `publish`.
 * Its bytes stay private so publication can write them without hashing or decoding them again.
 */
class PreparedShard final {
public:
    PreparedShard() noexcept = default;
    PreparedShard(const PreparedShard&) = delete;
    PreparedShard& operator=(const PreparedShard&) = delete;
    PreparedShard(PreparedShard&&) noexcept = default;
    PreparedShard& operator=(PreparedShard&&) noexcept = default;

    /** @return Exact serialized file size, including the header. */
    [[nodiscard]] std::uint64_t file_size() const noexcept {
        return static_cast<std::uint64_t>(header_.size())
               + static_cast<std::uint64_t>(payload_.size());
    }

    /** @return Digest of the serialized payload. */
    [[nodiscard]] const Digest& payload_sha256() const noexcept {
        return payloadSha256_;
    }

private:
    friend bool prepare(const Digest& sourceFingerprint,
                        const build_data::scriptables::Snapshot& snapshot,
                        PreparedShard& output) noexcept;
    friend bool
    publish(const wchar_t* path, PreparedShard prepared, Digest& payloadSha256) noexcept;

    std::vector<std::byte> header_{};
    std::vector<std::byte> payload_{};
    Digest payloadSha256_{};
};

/**
 * Serializes and hashes one complete shard exactly once.
 * The caller can use `payload_sha256` to choose the final digest-based path before publication.
 */
[[nodiscard]] bool prepare(const Digest& sourceFingerprint,
                           const build_data::scriptables::Snapshot& snapshot,
                           PreparedShard& output) noexcept;

/**
 * Publishes the exact bytes returned by `prepare` through a writer-owned sibling.
 * The writer checks writes, close, and atomic rename without rereading the trusted image.
 */
[[nodiscard]] bool
publish(const wchar_t* path, PreparedShard prepared, Digest& payloadSha256) noexcept;

/**
 * Computes the exact payload identity without touching the filesystem.
 * Use `prepare` when publication follows so the serialized bytes are retained.
 */
[[nodiscard]] bool payload_sha256(const Digest& sourceFingerprint,
                                  const build_data::scriptables::Snapshot& snapshot,
                                  Digest& output) noexcept;

/**
 * Writes one complete scenario snapshot through a writer-owned sibling.
 * The parent directory must already exist. The final path changes only after every byte is written
 * and the sibling closes cleanly. `load` authenticates a shard when it re-enters the process.
 * @param path Null-terminated final shard path selected by the caller.
 * @param sourceFingerprint Content-manifest build fingerprint that owns this shard.
 * @param snapshot Complete process snapshot for one nonzero scenario tag.
 * @param payloadSha256 Receives the committed payload digest only on success.
 * @return True when the complete shard is published under the final name.
 */
[[nodiscard]] bool write(const wchar_t* path,
                         const Digest& sourceFingerprint,
                         const build_data::scriptables::Snapshot& snapshot,
                         Digest& payloadSha256) noexcept;

/** Checks current format and manifest identity without reading or trusting payload contents. */
[[nodiscard]] bool compatible_header(const wchar_t* path,
                                     std::uint32_t expectedScenarioTag,
                                     const Digest& expectedSourceFingerprint,
                                     const Digest& expectedPayloadSha256) noexcept;

/**
 * Loads and validates one scenario shard without partially replacing the caller's snapshot.
 * @param path Null-terminated shard path selected by the caller.
 * @param expectedScenarioTag Exact nonzero scenario tag required from the header and payload.
 * @param expectedSourceFingerprint Exact content-manifest build fingerprint required by the file.
 * @param snapshot Replaced only after the whole shard validates and every vector is decoded.
 * @param payloadSha256 Receives the verified payload digest only on success.
 * @param status Receives the precise top-level load result.
 * @return True only when `status` is `loaded`.
 */
[[nodiscard]] bool load(const wchar_t* path,
                        std::uint32_t expectedScenarioTag,
                        const Digest& expectedSourceFingerprint,
                        build_data::scriptables::Snapshot& snapshot,
                        Digest& payloadSha256,
                        LoadStatus& status) noexcept;

} // namespace sunrise::state::activity_sdk::generated_world
