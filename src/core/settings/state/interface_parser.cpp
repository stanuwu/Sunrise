#include <bitset>

#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Parses HUD and text settings under stable Sunrise-owned names. */
bool Parser::interface_settings(state::account::settings::Interface& output) noexcept {
    enum class Field : std::size_t {
        subtitlesMode,
        colorblindMode,
        helmetMode,
        hudOpacity,
        displayHints,
        backgroundOpacity,
        reticleLocation,
        reticleColor,
        textSize,
        textColor,
        textBackgroundStyle,
        textBackgroundOpacity,
        reservedTextMode,
        subtitleOptionsEntry,
        count,
    };

    output = {};
    if (!consume('{')) {
        return false;
    }
    std::bitset<static_cast<std::size_t>(Field::count)> supplied;
    const auto mark = [&supplied](Field field) noexcept {
        const std::size_t index = static_cast<std::size_t>(field);
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
        if (key == "subtitles_mode") {
            if (!mark(Field::subtitlesMode) || !signed_byte(output.subtitlesMode)) {
                return false;
            }
        } else if (key == "colorblind_mode") {
            if (!mark(Field::colorblindMode) || !signed_byte(output.colorblindMode)) {
                return false;
            }
        } else if (key == "helmet_mode") {
            if (!mark(Field::helmetMode) || !signed_byte(output.helmetMode)) {
                return false;
            }
        } else if (key == "hud_opacity") {
            if (!mark(Field::hudOpacity) || !signed_byte(output.hudOpacity)) {
                return false;
            }
        } else if (key == "display_hints") {
            if (!mark(Field::displayHints) || !boolean(output.displayHints)) {
                return false;
            }
        } else if (key == "background_opacity") {
            if (!mark(Field::backgroundOpacity) || !signed_byte(output.backgroundOpacity)) {
                return false;
            }
        } else if (key == "reticle_location") {
            if (!mark(Field::reticleLocation) || !signed_byte(output.reticleLocation)) {
                return false;
            }
        } else if (key == "reticle_color") {
            if (!mark(Field::reticleColor) || !signed_byte(output.reticleColor)) {
                return false;
            }
        } else if (key == "text_size") {
            if (!mark(Field::textSize) || !signed_byte(output.textSize)) {
                return false;
            }
        } else if (key == "text_color") {
            if (!mark(Field::textColor) || !signed_byte(output.textColor)) {
                return false;
            }
        } else if (key == "text_background_style") {
            if (!mark(Field::textBackgroundStyle) || !signed_byte(output.textBackgroundStyle)) {
                return false;
            }
        } else if (key == "text_background_opacity") {
            if (!mark(Field::textBackgroundOpacity) || !signed_byte(output.textBackgroundOpacity)) {
                return false;
            }
        } else if (key == "reserved_text_mode") {
            if (!mark(Field::reservedTextMode) || !signed_byte(output.reservedTextMode)) {
                return false;
            }
        } else if (key == "subtitle_options_entry") {
            if (!mark(Field::subtitleOptionsEntry) || !signed_byte(output.subtitleOptionsEntry)) {
                return false;
            }
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return supplied.all();
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
