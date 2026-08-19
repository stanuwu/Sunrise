#include "catalog_record.h"

namespace sunrise::izanami::catalog {

/** Maps known Tiger class IDs to the logical object kind the editor should expose. */
core::ObjectKind object_kind_for_class(std::uint32_t classId) noexcept {
    if (classId == kStaticMeshClassId) {
        return core::ObjectKind::staticInstance;
    }
    if (classId == kPatternClassId) {
        return core::ObjectKind::patternInstance;
    }
    if (classId == kEntityClassId) {
        return core::ObjectKind::entityInstance;
    }
    return core::ObjectKind::forgeOnly;
}

} // namespace sunrise::izanami::catalog