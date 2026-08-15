#include <algorithm>
#include <array>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

namespace bindings = state::account::settings::bindings;

/** Stable JSON names follow the fixed authored action order in State. */
constexpr std::array<std::string_view, bindings::kActionCount> kActionNames{
    "fire",
    "toggle_zoom",
    "hold_zoom",
    "melee",
    "grenade",
    "super",
    "reload",
    "light_attack",
    "heavy_attack",
    "block",
    "switch_weapons",
    "next_weapon",
    "previous_weapon",
    "primary_weapon",
    "special_weapon",
    "heavy_weapon",
    "move_forward",
    "move_backward",
    "move_left",
    "move_right",
    "jump",
    "toggle_crouch",
    "hold_crouch",
    "toggle_sprint",
    "hold_sprint",
    "vehicle_boost",
    "vehicle_brake",
    "vehicle_zoom",
    "vehicle_fire_primary",
    "vehicle_fire_secondary",
    "vehicle_exit",
    "interact",
    "highlight_player",
    "emote_1",
    "emote_2",
    "emote_3",
    "emote_4",
    "air_move",
    "class_ability",
    "death_cam_zoom_in",
    "death_cam_zoom_out",
    "push_to_talk",
    "ui_gamepad_button_back",
    "ui_open_director",
    "ui_open_director_store_tab",
    "ui_open_director_pursuits_tab",
    "ui_open_director_map_tab",
    "ui_open_director_destinations_tab",
    "ui_open_director_roster_tab",
    "ui_open_director_seasons_tab",
    "ui_open_start_menu_alternative",
    "ui_open_start_menu_records_tab",
    "ui_open_start_menu_collections_tab",
    "ui_open_start_menu_clan_tab",
    "ui_open_start_menu_inventory_tab",
    "ui_open_start_menu_settings_tab",
    "ui_open_exit_dialog_confirm",
    "ui_abort_activity",
    "ui_text_chat_toggle_state",
    "screenshot",
};

/**
 * Finds one action name without building a map at boot.
 * @param name Borrowed JSON property name.
 * @return Fixed State index, or the action count when the name is unknown.
 */
[[nodiscard]] std::size_t action_index(std::string_view name) noexcept {
    const auto found = std::find(kActionNames.begin(), kActionNames.end(), name);
    return static_cast<std::size_t>(found - kActionNames.begin());
}

/** One authored input name and the input code it stands for. */
struct InputName {
    std::string_view name;
    std::uint16_t code;
};

/**
 * The Client's own input names and codes, read from its name table at `0x7FF7438A3400`. The names
 * are its strings. The last four rows are its own aliases, and the JSON escape for the backslash.
 */
constexpr std::array<InputName, 121> kInputNames{{
    {"escape", 0},
    {"f1", 1},
    {"f2", 2},
    {"f3", 3},
    {"f4", 4},
    {"f5", 5},
    {"f6", 6},
    {"f7", 7},
    {"f8", 8},
    {"f9", 9},
    {"f10", 10},
    {"f11", 11},
    {"f12", 12},
    {"print screen", 13},
    {"scroll lock", 14},
    {"pause", 15},
    {"`", 16},
    {"1", 17},
    {"2", 18},
    {"3", 19},
    {"4", 20},
    {"5", 21},
    {"6", 22},
    {"7", 23},
    {"8", 24},
    {"9", 25},
    {"0", 26},
    {"-", 27},
    {"=", 28},
    {"backspace", 29},
    {"tab", 30},
    {"q", 31},
    {"w", 32},
    {"e", 33},
    {"r", 34},
    {"t", 35},
    {"y", 36},
    {"u", 37},
    {"i", 38},
    {"o", 39},
    {"p", 40},
    {"[", 41},
    {"]", 42},
    {"\\", 43},
    {"caps lock", 44},
    {"a", 45},
    {"s", 46},
    {"d", 47},
    {"f", 48},
    {"g", 49},
    {"h", 50},
    {"j", 51},
    {"k", 52},
    {"l", 53},
    {";", 54},
    {"'", 55},
    {"return", 56},
    {"left shift", 57},
    {"z", 58},
    {"x", 59},
    {"c", 60},
    {"v", 61},
    {"b", 62},
    {"n", 63},
    {"m", 64},
    {",", 65},
    {".", 66},
    {"/", 67},
    {"right shift", 68},
    {"left control", 69},
    {"left windows", 70},
    {"left alt", 71},
    {"space", 72},
    {"right alt", 73},
    {"right windows", 74},
    {"menu", 75},
    {"right control", 76},
    {"up", 77},
    {"down", 78},
    {"left", 79},
    {"right", 80},
    {"insert", 81},
    {"home", 82},
    {"page up", 83},
    {"delete", 84},
    {"end", 85},
    {"page down", 86},
    {"num lock", 87},
    {"keypad /", 88},
    {"keypad *", 89},
    {"keypad 0", 90},
    {"keypad 1", 91},
    {"keypad 2", 92},
    {"keypad 3", 93},
    {"keypad 4", 94},
    {"keypad 5", 95},
    {"keypad 6", 96},
    {"keypad 7", 97},
    {"keypad 8", 98},
    {"keypad 9", 99},
    {"keypad -", 100},
    {"keypad +", 101},
    {"keypad enter", 102},
    {"keypad .", 103},
    {"<", 104},
    {"shift", 105},
    {"control", 106},
    {"key_windows", 107},
    {"alt", 108},
    {"left mouse button", 109},
    {"middle mouse button", 110},
    {"right mouse button", 111},
    {"extra mouse button 1", 112},
    {"extra mouse button 2", 113},
    {"mouse wheel up", 114},
    {"mouse wheel down", 115},
    {"unused", 116},
    {"ctrl", 106},
    {"left ctrl", 69},
    {"right ctrl", 76},
    {"\\\\", 43},
}};

/** A binding half carries its key code in the low byte and one modifier above it. */
constexpr std::uint16_t kAltFlag = 0x0100;
constexpr std::uint16_t kControlFlag = 0x0200;
constexpr std::uint16_t kShiftFlag = 0x0400;

/** One code that may prefix another key, and the flag it sets there. */
struct ModifierName {
    std::uint16_t code;
    std::uint16_t flag;
};

/** Both sides of a modifier fold onto the same flag, as they do in the Client. */
constexpr std::array<ModifierName, 9> kModifiers{{
    {57, kShiftFlag},
    {68, kShiftFlag},
    {105, kShiftFlag},
    {69, kControlFlag},
    {76, kControlFlag},
    {106, kControlFlag},
    {71, kAltFlag},
    {73, kAltFlag},
    {108, kAltFlag},
}};

/** @return The name without leading and trailing ASCII blanks. */
[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/** @return True when the two names match with ASCII case folded, as the Client compares them. */
[[nodiscard]] constexpr bool same_name(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char first = left[index];
        char second = right[index];
        first = (first >= 'A' && first <= 'Z') ? static_cast<char>(first + ('a' - 'A')) : first;
        second =
            (second >= 'A' && second <= 'Z') ? static_cast<char>(second + ('a' - 'A')) : second;
        if (first != second) {
            return false;
        }
    }
    return true;
}

/**
 * Finds one whole input name.
 * @param output Receives the code the table gives that name.
 * @return True when the name is in the table.
 */
[[nodiscard]] bool named_code(std::string_view name, std::uint16_t& output) noexcept {
    for (const InputName& entry : kInputNames) {
        if (same_name(entry.name, name)) {
            output = entry.code;
            return true;
        }
    }
    return false;
}

/**
 * @param code Code parsed from a prefix name.
 * @param output Receives the flag that code sets on the key it prefixes.
 * @return True when the code is a modifier.
 */
[[nodiscard]] bool modifier_flag(std::uint16_t code, std::uint16_t& output) noexcept {
    for (const ModifierName& entry : kModifiers) {
        if (entry.code == code) {
            output = entry.flag;
            return true;
        }
    }
    return false;
}

} // namespace

/** Parses the whole fixed action table under named JSON properties. */
bool Parser::key_bindings(bindings::KeyBindings& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    std::array<bool, bindings::kActionCount> present{};
    std::size_t presentCount = 0;
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        const std::size_t index = action_index(key);
        if (index == bindings::kActionCount) {
            if (!skip_value(0)) {
                return false;
            }
        } else {
            if (present[index] || !key_binding(output.values[index])) {
                return false;
            }
            present[index] = true;
            ++presentCount;
        }
        if (consume('}')) {
            output.configured = presentCount == bindings::kActionCount;
            return output.configured;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one binding with explicit primary and secondary halves. */
bool Parser::key_binding(bindings::Binding& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    bool hasPrimary = false;
    bool hasSecondary = false;
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "primary") {
            if (hasPrimary || !optional_input_code(output.primary)) {
                return false;
            }
            hasPrimary = true;
        } else if (key == "secondary") {
            if (hasSecondary || !optional_input_code(output.secondary)) {
                return false;
            }
            hasSecondary = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return hasPrimary && hasSecondary;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one optional input name. Null is the unbound half. */
bool Parser::optional_input_code(std::optional<std::uint16_t>& output) noexcept {
    if (literal("null")) {
        output.reset();
        return true;
    }
    std::string_view name;
    std::uint16_t code = 0;
    if (!string(name) || !input_code_value(name, code)) {
        return false;
    }
    output = code;
    return true;
}

/** Turns one input name into the code the Client reads. */
bool Parser::input_code_value(std::string_view name, std::uint16_t& output) noexcept {
    const std::string_view text = trim(name);
    if (text.empty()) {
        return false;
    }
    if (named_code(text, output)) {
        return true;
    }
    // Anything else is one modifier, then the key it prefixes. The key may hold a separator.
    const std::size_t separator = text.find_first_of("+-");
    if (separator == std::string_view::npos) {
        return false;
    }
    std::uint16_t prefix = 0;
    std::uint16_t flag = 0;
    if (!named_code(trim(text.substr(0, separator)), prefix) || !modifier_flag(prefix, flag)) {
        return false;
    }
    std::uint16_t key = 0;
    if (!named_code(trim(text.substr(separator + 1)), key)) {
        return false;
    }
    output = static_cast<std::uint16_t>(flag | key);
    return true;
}

} // namespace sunrise::core::settings::parser
