#include "../../ui/runtime/ui_toggle_keys.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Parses the in-game UI boot and input policy. */
bool Parser::client_ui_settings(ui::runtime::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    ui::runtime::Settings candidate = output;
    bool hasEnabled = false;
    bool hasToggleKey = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "enabled") {
            if (hasEnabled || !boolean(candidate.enabled)) {
                return false;
            }
            hasEnabled = true;
        } else if (key == "toggle_key") {
            std::string_view name;
            if (hasToggleKey || !string(name)
                || !ui::runtime::toggle_key_value(name, candidate.toggleVirtualKey)) {
                return false;
            }
            hasToggleKey = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
