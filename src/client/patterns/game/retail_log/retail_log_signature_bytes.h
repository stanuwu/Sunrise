#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../../registry.h"
#include "../../signature_text.h"

namespace sunrise::client::patterns::game::retail_log {

/** Single funnel every retail log line passes through, already formatted. */
inline constexpr std::string_view kEnqueueText = "83 F9 FF 74 ? 48 89 5C 24 ? 56";

/** Per-category threshold setter; a lower threshold is more verbose. */
inline constexpr std::string_view kSetCategoryVerbosityText =
    "40 55 53 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 "
    "? ? ? ? 48 63 F9 8B DA";

/** Retail-log enqueue signature byte count. */
inline constexpr std::size_t kEnqueuePatternSize = signature_length(kEnqueueText);
/** Retail-log category-verbosity signature byte count. */
inline constexpr std::size_t kSetCategoryVerbosityPatternSize =
    signature_length(kSetCategoryVerbosityText);

extern constinit const std::array<patterns::PatternByte, kEnqueuePatternSize> kEnqueue;
extern constinit const std::array<patterns::PatternByte, kSetCategoryVerbosityPatternSize>
    kSetCategoryVerbosity;

} // namespace sunrise::client::patterns::game::retail_log
