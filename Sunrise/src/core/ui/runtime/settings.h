#pragma once

#include <Windows.h>

#include <cstdint>

namespace sunrise::core::ui::runtime {

/** Most rows `missionVariableRows` may ask for; the variable log retains this many changes. */
inline constexpr std::uint32_t kMissionVariableRowsMaximum = 48;

/** In-game UI settings, read at boot. */
struct Settings {
    /** When off, the UI ignores the toggle key. */
    bool enabled{true};
    /** Windows virtual key that shows or hides the UI. */
    UINT toggleVirtualKey{VK_INSERT};
    /** Newest mission variable changes the Mission Variables overlay lists, 1 to the maximum. */
    std::uint32_t missionVariableRows{15};
};

} // namespace sunrise::core::ui::runtime
