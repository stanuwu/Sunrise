#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../state/account/inventory/seen_state.h"
#include "../../../state/social/native_presence.h"
#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode702 {

/** Web Service opcode of the character object B write-back. */
inline constexpr std::uint16_t kOpcode = 702;
/** The 5360-byte character mirror packs into at most 4800 bytes; shorter bodies are valid. */
inline constexpr std::size_t kPayloadSize = 4800;

/** Supported fields from the character writeback. */
struct Request {
    std::optional<state::account::inventory::CharacterNewItems> newItems;
    state::social::NativePresence presence{};
};

/**
 * Reads the complete character writeback with optional groups and bounded zero padding.
 * @param message Parsed Web Service envelope.
 * @param request Receives only fields present in a valid body.
 * @return False for malformed or truncated fields.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode702
