#include "forge_object.h"

namespace sunrise::izanami::project::scene {

/** Builds one logical Forge scene object without assigning any native runtime binding. */
ForgeObject make_object(core::ForgeUUID id,
                        core::ObjectKind kind,
                        core::ResourceId resource,
                        core::Transform transform) {
    ForgeObject object;
    object.id = id;
    object.kind = kind;
    object.resource = resource;
    object.transform = transform;
    return object;
}

/** Builds one logical static instance record. */
StaticInstance make_static_instance(core::ForgeUUID id,
                                    core::ResourceId resource,
                                    core::Transform transform) {
    return {.object = make_object(id, core::ObjectKind::staticInstance, resource, transform)};
}

/** Builds one logical pattern instance record. */
PatternInstance make_pattern_instance(core::ForgeUUID id,
                                      core::ResourceId resource,
                                      core::Transform transform) {
    return {.object = make_object(id, core::ObjectKind::patternInstance, resource, transform)};
}

} // namespace sunrise::izanami::project::scene