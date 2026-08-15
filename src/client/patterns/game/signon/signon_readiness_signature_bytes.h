#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../../registry.h"
#include "../../signature_text.h"

namespace sunrise::client::patterns::game::signon {

/** Pre-SignOn native transport-readiness predicate signature. */
inline constexpr std::string_view kReadinessFailureText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 05 ? ? ? ? 40 32 FF";

/** Pre-SignOn native transport-ready predicate signature. */
inline constexpr std::string_view kReadinessReadyText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 05 ? ? ? ? 33 F6 48 8B 15 ? ? ? ? 8B DE "
    "40 B7 01";

/** Pre-SignOn readiness predicate signature byte count. */
inline constexpr std::size_t kReadinessFailurePatternSize = signature_length(kReadinessFailureText);
/** Pre-SignOn ready predicate signature byte count. */
inline constexpr std::size_t kReadinessReadyPatternSize = signature_length(kReadinessReadyText);

extern constinit const std::array<patterns::PatternByte, kReadinessFailurePatternSize>
    kReadinessFailure;
extern constinit const std::array<patterns::PatternByte, kReadinessReadyPatternSize>
    kReadinessReady;

} // namespace sunrise::client::patterns::game::signon
