#include "ui_toggle_keys.h"

#include <array>

namespace sunrise::core::ui::runtime {
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

/** Maps one settings key name to a Windows virtual key from the supported toggle set. */
bool toggle_key_value(std::string_view name, UINT& output) noexcept {
    for (const KeyName& entry : kToggleKeys) {
        if (entry.name == name) {
            output = entry.virtualKey;
            return true;
        }
    }
    return false;
}

/** Names one Windows virtual key with the setting that selects it. */
std::string_view toggle_key_name(UINT virtualKey) noexcept {
    for (const KeyName& entry : kToggleKeys) {
        if (entry.virtualKey == virtualKey) {
            return entry.name;
        }
    }
    return {};
}

} // namespace sunrise::core::ui::runtime
