#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::replication_epoch {

/** Activity message 44 carries one generation byte and one zero latch byte. */
inline constexpr std::size_t kEncodedSize = 2;

/**
 * Encodes activity message 44 with the give-up latch byte forced to zero.
 * A set latch byte permanently kills the client's roster path, so nothing else may build this body.
 * TODO: no sender yet. The common-generation barrier has to be measured before this is called.
 */
[[nodiscard]] bool
encode(std::uint8_t generation, std::span<std::byte> output, std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::activity_message::replication_epoch
