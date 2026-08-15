#pragma once

#include <array>
#include <cstddef>

namespace sunrise::core::settings::steam {

/** Steam persona policy allows at most 63 printable ASCII bytes. */
inline constexpr std::size_t kMaximumPersonaNameBytes = 63;
/** Fixed persona storage includes one trailing null byte. */
inline constexpr std::size_t kPersonaNameCapacity = kMaximumPersonaNameBytes + 1;

/** Read-only settings for the single local Steam user. */
struct User {
    /** Process-owned persona storage. Defaults to a neutral made-up name. */
    std::array<char, kPersonaNameCapacity> personaName{"Player"};
};

/** Read-only Steam compatibility settings parsed by Core. */
struct Settings {
    /** Options for the single local user exposed through Steam interfaces. */
    User user;
};

} // namespace sunrise::core::settings::steam
