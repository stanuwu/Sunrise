#include "signon_readiness_signature_bytes.h"

namespace sunrise::client::patterns::game::signon {

constinit const std::array<patterns::PatternByte, kReadinessFailurePatternSize> kReadinessFailure =
    signature<kReadinessFailurePatternSize>(kReadinessFailureText);

constinit const std::array<patterns::PatternByte, kReadinessReadyPatternSize> kReadinessReady =
    signature<kReadinessReadyPatternSize>(kReadinessReadyText);

} // namespace sunrise::client::patterns::game::signon
