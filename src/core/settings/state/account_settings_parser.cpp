#include <bitset>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** Required account-setting groups use one fixed presence slot each. */
enum class Group : std::size_t {
    controls,
    audio,
    display,
    interface,
    social,
    keyBindings,
    count,
};

} // namespace

/** Parses grouped account settings. Native record offsets stay hidden. */
bool Parser::account_settings(state::account::settings::AccountSettings& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    std::bitset<static_cast<std::size_t>(Group::count)> supplied;
    const auto mark = [&supplied](Group group) noexcept {
        const std::size_t index = static_cast<std::size_t>(group);
        if (supplied.test(index)) {
            return false;
        }
        supplied.set(index);
        return true;
    };
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "controls") {
            if (!mark(Group::controls) || !controls_settings(output.controls)) {
                return false;
            }
        } else if (key == "audio") {
            if (!mark(Group::audio) || !audio_settings(output.audio)) {
                return false;
            }
        } else if (key == "display") {
            if (!mark(Group::display) || !display_settings(output.display)) {
                return false;
            }
        } else if (key == "interface") {
            if (!mark(Group::interface) || !interface_settings(output.interface)) {
                return false;
            }
        } else if (key == "social") {
            if (!mark(Group::social) || !social_settings(output.social)) {
                return false;
            }
        } else if (key == "key_bindings") {
            if (!mark(Group::keyBindings) || !key_bindings(output.keyBindings)) {
                return false;
            }
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            if (!supplied.all()) {
                return false;
            }
            output.configured = true;
            return state::account::settings::valid(output);
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
