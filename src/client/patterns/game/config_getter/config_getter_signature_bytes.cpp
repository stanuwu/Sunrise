#include "config_getter_signature_bytes.h"

namespace sunrise::client::patterns::game::config_getter {

// The jump, its stack fixup tail, and the two following jumps that make the thunk unique.
constinit const std::array<patterns::PatternByte, kUrlGetterPatternSize> kUrlGetter =
    signature<kUrlGetterPatternSize>(kUrlGetterText);

// The jump, the negative-one load its caller tests, and the following jumps.
constinit const std::array<patterns::PatternByte, kTokenGetterPatternSize> kTokenGetter =
    signature<kTokenGetterPatternSize>(kTokenGetterText);

} // namespace sunrise::client::patterns::game::config_getter
