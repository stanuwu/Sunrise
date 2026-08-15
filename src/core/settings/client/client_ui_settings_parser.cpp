#include <Windows.h>

#include <array>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** One readable configuration name and its Windows virtual-key value. */
struct KeyName {
    std::string_view name;
    UINT virtualKey;
};

/** Keys offered as safe menu toggles without inventing a private input-code domain. */
constexpr std::array kToggleKeys{
    KeyName{"insert", VK_INSERT},
    KeyName{"home", VK_HOME},
    KeyName{"end", VK_END},
    KeyName{"delete", VK_DELETE},
    KeyName{"f1", VK_F1},
    KeyName{"f2", VK_F2},
    KeyName{"f3", VK_F3},
    KeyName{"f4", VK_F4},
    KeyName{"f5", VK_F5},
    KeyName{"f6", VK_F6},
    KeyName{"f7", VK_F7},
    KeyName{"f8", VK_F8},
    KeyName{"f9", VK_F9},
    KeyName{"f10", VK_F10},
    KeyName{"f11", VK_F11},
    KeyName{"f12", VK_F12},
};

} // namespace

/** Maps one readable setting to the Windows SDK virtual-key domain. */
bool Parser::ui_toggle_key_value(std::string_view name, UINT& output) noexcept {
    for (const KeyName& entry : kToggleKeys) {
        if (entry.name == name) {
            output = entry.virtualKey;
            return true;
        }
    }
    return false;
}

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
                || !ui_toggle_key_value(name, candidate.toggleVirtualKey)) {
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
