#pragma once

#include <cstdint>

#include "../../../../state/account/settings/settings_delta.h"
#include "../../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode701 {

/** Web Service opcode used by the Client's account-settings writeback. */
inline constexpr std::uint16_t kOpcode = 701;

/** Semantic result decoded from one schema-0x80807603 request. */
struct Request {
    state::account::settings::SettingsDelta settings;
};

/**
 * Decodes the complete presence-driven opcode-701 request body.
 *
 * Unsupported schema branches are still traversed so every later field is read at its actual
 * wire position. Output is cleared on entry; decoded values replace it only after the entire
 * schema, optional outer blobs, and zero terminal padding validate.
 *
 * @param message Parsed Web Service envelope whose payload begins at schema bit zero.
 * @param output Receives supported fields, the authored binding source, and the atomic table.
 * @return True only when opcode and complete request encoding are valid.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& output) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode701
