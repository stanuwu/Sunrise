#include <limits>

#include "../../../state/entitlements/validation.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Checks the standalone Server settings object. */
bool Parser::server_settings(server::Settings& output) noexcept {
    output = {};
    output.entitlements = state::entitlements::authored();
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    bool hasEntitlements = false;
    bool hasBapPort = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "entitlements") {
            if (hasEntitlements || !entitlements(output.entitlements)) {
                return false;
            }
            hasEntitlements = true;
        } else if (key == "bap_port") {
            std::uint64_t value = 0;
            if (hasBapPort || !unsigned_integer(value) || value == 0
                || value > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            output.bapPort = static_cast<std::uint16_t>(value);
            hasBapPort = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return state::entitlements::valid(output.entitlements);
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
