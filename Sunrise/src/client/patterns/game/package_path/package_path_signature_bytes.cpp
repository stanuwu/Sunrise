#include "package_path_signature_bytes.h"

namespace sunrise::client::patterns::game::package_path {

// The whole builder: `.pkg` test, path field address, verbatim copy, then the formatted path.
constinit const std::array<patterns::PatternByte, kSetPathPatternSize> kSetPath =
    signature<kSetPathPatternSize>(kSetPathText);

// Opener prologue, the resolve into a stack buffer, then the call that opens the text.
constinit const std::array<patterns::PatternByte, kOpenFilePatternSize> kOpenFile =
    signature<kOpenFilePatternSize>(kOpenFileText);

// Resolver prologue with its root 11 and skip-lookup flag tests.
constinit const std::array<patterns::PatternByte, kResolvePathPatternSize> kResolvePath =
    signature<kResolvePathPatternSize>(kResolvePathText);

// Dispatcher prologue up to its callback table lookup.
constinit const std::array<patterns::PatternByte, kSchemaCallbackPatternSize> kSchemaCallback =
    signature<kSchemaCallbackPatternSize>(kSchemaCallbackText);

} // namespace sunrise::client::patterns::game::package_path
