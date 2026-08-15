#include "activity_client_keepalive_validator.h"

namespace sunrise::middleware::bap::activity_message::client_keepalive {

/** Checks one opaque client keepalive body without reading its byte. */
bool validate_client_keepalive(std::span<const std::byte> input) noexcept {
    return input.size() == kEncodedSize;
}

} // namespace sunrise::middleware::bap::activity_message::client_keepalive
