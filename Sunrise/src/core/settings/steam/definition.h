#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::core::settings::steam {

/**
 * The identity this build falls back to when none is configured and none was created.
 * Public universe, individual account type and desktop instance in the high dword, then one fixed
 * account number. Created identities keep that shape and reuse its low byte.
 */
inline constexpr std::uint64_t kDefaultSteamId = 0x0110000130AA9EC5ULL;

/** Steam persona policy allows at most 63 printable ASCII bytes. */
inline constexpr std::size_t kMaximumPersonaNameBytes = 63;
/** Fixed persona storage includes one trailing null byte. */
inline constexpr std::size_t kPersonaNameCapacity = kMaximumPersonaNameBytes + 1;
/** Comfortably longer than any published Steam API language token (e.g. "vietnamese"). */
inline constexpr std::size_t kMaximumLanguageBytes = 15;
/** Fixed language storage includes one trailing null byte. */
inline constexpr std::size_t kLanguageCapacity = kMaximumLanguageBytes + 1;

/** Read-only settings for the single local Steam user. */
struct User {
    /** Process-owned persona storage. Defaults to a neutral made-up name. */
    std::array<char, kPersonaNameCapacity> personaName{"Player"};
    std::uint64_t steamId{kDefaultSteamId};
    /** An explicit identity takes precedence over automatic co-op identity creation. */
    bool hasConfiguredSteamId{};
};

/** Read-only Steam compatibility settings parsed by Core. */
struct Settings {
    /** Options for the single local user exposed through Steam interfaces. */
    User user;
    /** Steam API language token answered for GetCurrentGameLanguage/GetAvailableGameLanguages. */
    std::array<char, kLanguageCapacity> language{"english"};
};

} // namespace sunrise::core::settings::steam
