#pragma once

#include <cstddef>
#include <string_view>

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::external_server {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/** `curl_easy_setopt(handle, option, value)`, with the value in r8. */
inline constexpr std::string_view kSetoptSignatureText =
    "89 54 24 ? 4C 89 44 24 ? 4C 89 4C 24 ? 48 83 EC ? 48 85 C9";
/** Compiled pattern bytes of the signature text above. */
inline constexpr auto kSetoptSignature =
    signature<signature_length(kSetoptSignatureText)>(kSetoptSignatureText);

/** curl option ids, stable across curl 7.x. */
inline constexpr int kSslVerifyPeer = 64;
inline constexpr int kSslVerifyHost = 81;
inline constexpr int kUrl = 10002;

/** The Client's own inline URL field is 512 bytes, and no request URL is longer. */
inline constexpr std::size_t kUrlCapacity = 512;

} // namespace sunrise::client::hooks::external_server
