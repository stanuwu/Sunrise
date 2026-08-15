#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../../registry.h"
#include "../../signature_text.h"

namespace sunrise::client::patterns::game::assert_handler {

/**
 * Whole body of the setter that writes the assert handler slot. Several copies are byte-identical
 * apart from the displacement, so a match is a candidate, not an answer.
 */
inline constexpr std::string_view kSetHandlerText =
    "48 89 4C 24 ? 57 48 8B 44 24 ? 48 89 05 ? ? ? ? 5F C3";

/**
 * Tail of every assert site's fatal branch: the report call, then a deliberate write to a null
 * address. It repeats once per assert site and appears nowhere else.
 */
inline constexpr std::string_view kFatalTailText = "E8 ? ? ? ? C7 04 25 01 00 00 00 2A 00 00 00";

/** Handler-slot setter signature byte count. */
inline constexpr std::size_t kSetHandlerPatternSize = signature_length(kSetHandlerText);
/** Assert-site fatal-tail signature byte count. */
inline constexpr std::size_t kFatalTailPatternSize = signature_length(kFatalTailText);

extern constinit const std::array<patterns::PatternByte, kSetHandlerPatternSize> kSetHandler;
extern constinit const std::array<patterns::PatternByte, kFatalTailPatternSize> kFatalTail;

} // namespace sunrise::client::patterns::game::assert_handler
