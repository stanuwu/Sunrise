#include "schema.h"

namespace sunrise::izanami::project::serialization {

/** @return True when this build can read the project schema version. */
bool is_supported_project_version(std::uint32_t version) noexcept {
    return version > 0 && version <= kProjectFormatVersion;
}

} // namespace sunrise::izanami::project::serialization