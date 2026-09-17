#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../social/native_presence.h"

namespace sunrise::state {

/** ASCII bytes including NUL; each expands to one of the directory's 27 UTF-16 name units. */
inline constexpr std::size_t kDisplayNameCapacity = 27;
/** Four ASCII name-code characters plus NUL, expanded to UTF-16 in the directory record. */
inline constexpr std::size_t kNameCodeCapacity = 5;
/** The three low bits of that record's flag byte. A profile setting a higher bit is refused. */
inline constexpr std::uint8_t kPresenceFlagsMask = 0x07;

/**
 * One account's published presence. `personaName` is the live platform name; `displayName`
 * is the name last published and what staleness checks compare. A local snapshot sets them
 * equal.
 */
struct AccountPresence {
    std::array<char, kDisplayNameCapacity> displayName{};
    std::array<char, kDisplayNameCapacity> personaName{};
    std::uint64_t platformId{};
    std::array<char, kNameCodeCapacity> nameCode{};
    std::uint8_t flags{};
    /** Public displayed Power contribution; the private artifact banks stay on the client. */
    std::uint16_t artifactPowerBonus{};
    social::NativePresence native{};
};

} // namespace sunrise::state
