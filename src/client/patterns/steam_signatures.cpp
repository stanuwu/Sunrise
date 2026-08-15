#include <array>
#include <string_view>

#include "signature_text.h"
#include "steam.h"

namespace sunrise::client::patterns::steam {
namespace {

// Matches the authentication availability reader used by Client startup.
constexpr std::string_view kAuthenticationStatusText =
    "40 57 48 83 EC ? 48 C7 44 24 ? ? ? ? ? 48 89 5C 24 ? 48 8B DA 48 8B F9 33 C9";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kAuthenticationStatus =
    signature<signature_length(kAuthenticationStatusText)>(kAuthenticationStatusText);

// Matches the certificate parser whose verdict gates BAP setup. The match runs on into the
// register homes because the prologue alone is not unique once the frame values are wildcarded.
constexpr std::string_view kSetCertificateText =
    "48 8B C4 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 48 C7 44 24 ? FE FF FF "
    "FF 48 89 58 ? 48 89 70 ? 48 89 78 ? 4D 8B F1 41 8B D8 48 8B FA 48 8B F1";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSetCertificate =
    signature<signature_length(kSetCertificateText)>(kSetCertificateText);

/** Steam signatures in the exact target-slot order. */
constexpr std::array kDefinitions{
    patterns::Pattern{"authentication_status", kAuthenticationStatus},
    patterns::Pattern{"set_certificate", kSetCertificate},
};

static_assert(kDefinitions.size() == static_cast<std::size_t>(Id::count));

} // namespace

/** @return Immutable Steam signature definitions in target-slot order. */
std::span<const patterns::Pattern> definitions() noexcept {
    return kDefinitions;
}

} // namespace sunrise::client::patterns::steam
