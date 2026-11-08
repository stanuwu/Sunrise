#pragma once

#include <cstdint>
#include <optional>

#include "../../../../state/account/inventory/seen_state.h"
#include "../../../../state/account/settings/settings_delta.h"
#include "../../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode701 {

/** Web Service opcode used by the Client's account-settings writeback. */
inline constexpr std::uint16_t kOpcode = 701;

/** Semantic result decoded from one opcode-701 request. */
struct Request {
    state::account::settings::SettingsDelta settings;
    std::optional<state::account::inventory::ProfileNewItems> newItems;
    /** Preference path 0.1.1.0; false when the body leaves that preference absent. */
    bool profileSetupCompleted{};
};

/**
 * Decodes the complete presence-driven opcode-701 request body.
 * Unsupported branches are still traversed so later fields read at their real wire position.
 * Output is replaced only after the whole schema, the outer blobs and the padding validate.
 * @param message Parsed Web Service envelope whose payload begins at schema bit zero.
 * @param output Receives supported fields, the authored binding source, and the atomic table.
 * @return True only when opcode and complete request encoding are valid.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& output) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode701
