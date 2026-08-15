#include "package_key_signature_bytes.h"

namespace sunrise::client::patterns::game::packages {

// The load is followed by a fixed stack-adjust and tail branch, which is what makes it unique.
constinit const std::array<patterns::PatternByte, kKeyTablePatternSize> kKeyTable =
    signature<kKeyTablePatternSize>(kKeyTableText);

} // namespace sunrise::client::patterns::game::packages
