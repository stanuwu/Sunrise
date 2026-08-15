#pragma once

#include <array>
#include <cstddef>

namespace sunrise::core::settings::client::external {

/** An IPv4 dotted quad and its trailing null byte. */
inline constexpr std::size_t kHostCapacity = 16;
/** Octets in one IPv4 address. */
inline constexpr std::size_t kAddressOctets = 4;
/** Config URL storage, well inside the Client's own fixed URL field. */
inline constexpr std::size_t kConfigUrlCapacity = 128;
/** The Client copies the config token as 36 bytes and a null. */
inline constexpr std::size_t kConfigGuidCapacity = 37;

/**
 * Points the Client at a server outside this process.
 * The defaults name a local host on 443 for SignOn and the config manifest.
 */
struct Settings {
    /** Off answers SignOn, the config manifest, and BAP in process. */
    bool enabled{false};
    /** Numeric redirect target for every name lookup. */
    std::array<char, kHostCapacity> host{"127.0.0.1"};
    /** Wide copy of the host, for the wide resolver entries. */
    std::array<wchar_t, kHostCapacity> hostWide{L"127.0.0.1"};
    /** Redirect target octets in dotted-quad order. */
    std::array<unsigned char, kAddressOctets> address{127, 0, 0, 1};
    /** Answered to the Client's config URL getter. Must be a route the external server serves. */
    std::array<char, kConfigUrlCapacity> configUrl{"https://127.0.0.1/config/"};
    /** Answered to the config token getter. The Client compares it against manifest field 5. */
    std::array<char, kConfigGuidCapacity> configGuid{"d2legacy-0000-0000-0000-000000000001"};
};

} // namespace sunrise::core::settings::client::external
