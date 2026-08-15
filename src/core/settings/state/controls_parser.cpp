#include <bitset>

#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Parses controller and mouse settings under stable Sunrise-owned names. */
bool Parser::controls_settings(state::account::settings::Controls& output) noexcept {
    enum class Field : std::size_t {
        buttonLayout,
        movementMode,
        controllerLookSensitivity,
        controllerInvertVertical,
        controllerAutoLookCentering,
        controllerVibration,
        controllerSwapShoulders,
        controllerInvertHorizontal,
        mouseLookSensitivity,
        mouseInvertVertical,
        mouseInvertHorizontal,
        unidentifiedToggle,
        mouseAimSmoothing,
        adsSensitivityModifier,
        doublePressDelay,
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
        if (key == "button_layout") {
            if (!mark(Field::buttonLayout) || !signed_byte(output.buttonLayout)) {
                return false;
            }
        } else if (key == "movement_mode") {
            if (!mark(Field::movementMode) || !signed_byte(output.movementMode)) {
                return false;
            }
        } else if (key == "controller_look_sensitivity") {
            if (!mark(Field::controllerLookSensitivity)
                || !signed_byte(output.controllerLookSensitivity)) {
                return false;
            }
        } else if (key == "controller_invert_vertical") {
            if (!mark(Field::controllerInvertVertical)
                || !boolean(output.controllerInvertVertical)) {
                return false;
            }
        } else if (key == "controller_auto_look_centering") {
            if (!mark(Field::controllerAutoLookCentering)
                || !boolean(output.controllerAutoLookCentering)) {
                return false;
            }
        } else if (key == "controller_vibration") {
            if (!mark(Field::controllerVibration) || !boolean(output.controllerVibration)) {
                return false;
            }
        } else if (key == "controller_swap_shoulders") {
            if (!mark(Field::controllerSwapShoulders) || !boolean(output.controllerSwapShoulders)) {
                return false;
            }
        } else if (key == "controller_invert_horizontal") {
            if (!mark(Field::controllerInvertHorizontal)
                || !boolean(output.controllerInvertHorizontal)) {
                return false;
            }
        } else if (key == "mouse_look_sensitivity") {
            if (!mark(Field::mouseLookSensitivity) || !signed_32(output.mouseLookSensitivity)) {
                return false;
            }
        } else if (key == "mouse_invert_vertical") {
            if (!mark(Field::mouseInvertVertical) || !boolean(output.mouseInvertVertical)) {
                return false;
            }
        } else if (key == "mouse_invert_horizontal") {
            if (!mark(Field::mouseInvertHorizontal) || !boolean(output.mouseInvertHorizontal)) {
                return false;
            }
        } else if (key == "unidentified_toggle") {
            if (!mark(Field::unidentifiedToggle) || !boolean(output.unidentifiedToggle)) {
                return false;
            }
        } else if (key == "mouse_aim_smoothing") {
            if (!mark(Field::mouseAimSmoothing) || !boolean(output.mouseAimSmoothing)) {
                return false;
            }
        } else if (key == "ads_sensitivity_modifier") {
            if (!mark(Field::adsSensitivityModifier)
                || !floating_point(output.adsSensitivityModifier)) {
                return false;
            }
        } else if (key == "double_press_delay") {
            if (!mark(Field::doublePressDelay) || !signed_byte(output.doublePressDelay)) {
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
