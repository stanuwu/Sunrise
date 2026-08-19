#include "activity_replication_epoch_encoder.h"

namespace sunrise::middleware::bap::activity_message::replication_epoch {

/** Encodes activity message 44 with the give-up latch byte forced to zero. */
bool encode(std::uint8_t generation, std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kEncodedSize) {
        return false;
    }
    output[0] = static_cast<std::byte>(generation);
    output[1] = std::byte{};
    written = kEncodedSize;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::replication_epoch
