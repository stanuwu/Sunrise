#include "retail_log_signature_bytes.h"

namespace sunrise::client::patterns::game::retail_log {

// Matches the site-id guard that opens the enqueue funnel.
constinit const std::array<patterns::PatternByte, kEnqueuePatternSize> kEnqueue =
    signature<kEnqueuePatternSize>(kEnqueueText);

// Matches the verbosity setter through its category and level argument moves.
constinit const std::array<patterns::PatternByte, kSetCategoryVerbosityPatternSize>
    kSetCategoryVerbosity = signature<kSetCategoryVerbosityPatternSize>(kSetCategoryVerbosityText);

} // namespace sunrise::client::patterns::game::retail_log
