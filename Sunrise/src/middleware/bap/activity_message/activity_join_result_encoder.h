#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::join_result {

/** The known minimal local join-result encoding is 757 bytes. */
inline constexpr std::size_t kEncodedSize = 757;
/** Replication epoch authored by the minimal accepted join result. */
inline constexpr std::uint8_t kInitialReplicationEpoch = 0;

/**
 * Encodes the minimal accepted join result from typed request scalars.
 * The two millisecond hints are in wire order and share a byte. Do not swap them: both are
 * `uint16_t`, so an exchanged pair still compiles.
 * @param correlation Join-request correlation, written big-endian.
 * @param sessionId Allocated activity session id. Must not be zero.
 * @param peerHeardWindowMs 16-bit peer-heard window in milliseconds. Zero clears every peer bit.
 * @param keepaliveHintMs 16-bit keepalive hint in milliseconds.
 * @param replicationEpoch Current session-wide replication generation.
 * @param output Caller-owned payload storage, unchanged on failure.
 * @param written Receives 757 on success, or zero on failure.
 * @return True when the session id is valid and the whole body fits.
 */
[[nodiscard]] bool encode_join_result(std::uint32_t correlation,
                                      std::uint64_t sessionId,
                                      std::uint16_t peerHeardWindowMs,
                                      std::uint16_t keepaliveHintMs,
                                      std::uint8_t replicationEpoch,
                                      std::span<std::byte> output,
                                      std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::activity_message::join_result
