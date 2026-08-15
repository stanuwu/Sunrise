#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::join_result {

/** The known minimal local join-result encoding is 757 bytes. */
inline constexpr std::size_t kEncodedSize = 757;

/**
 * Encodes the minimal accepted join result from typed request scalars. The caller hint and the
 * zero-filled remaining fields are our own choice, not retail parity.
 * @param correlation Join-request correlation, written big-endian.
 * @param sessionId Allocated activity session id. Must not be zero.
 * @param keepaliveHintMs 16-bit keepalive hint in milliseconds.
 * @param output Caller-owned payload storage, unchanged on failure.
 * @param written Receives 757 on success, or zero on failure.
 * @return True when the session id is valid and the whole body fits.
 */
[[nodiscard]] bool encode_join_result(std::uint32_t correlation,
                                      std::uint64_t sessionId,
                                      std::uint16_t keepaliveHintMs,
                                      std::span<std::byte> output,
                                      std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::activity_message::join_result
