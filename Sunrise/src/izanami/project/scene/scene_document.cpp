#include "scene_document.h"

#include <algorithm>

namespace sunrise::izanami::project::scene {

namespace {

[[nodiscard]] bool same_id(const ForgeObject& object, core::ForgeUUID id) noexcept {
    return object.id == id;
}

} // namespace

/** Removes every logical scene object. */
void SceneDocument::clear() {
    objects_.clear();
}

/** Adds one logical object when its identity is unique and its transform is sane. */
bool SceneDocument::create(ForgeObject object) {
    if (object.id.is_nil() || !object.transform.is_finite() || contains(object.id)) {
        return false;
    }
    if (!object.parent.is_nil() && !contains(object.parent)) {
        return false;
    }
    objects_.push_back(std::move(object));
    return true;
}

/** Deletes one object and leaves any children at the scene root. */
bool SceneDocument::erase(core::ForgeUUID id) {
    const auto found = std::find_if(objects_.begin(), objects_.end(), [id](const ForgeObject& object) {
        return same_id(object, id);
    });
    if (found == objects_.end()) {
        return false;
    }
    objects_.erase(found);
    for (ForgeObject& object : objects_) {
        if (object.parent == id) {
            object.parent = {};
        }
    }
    return true;
}

/** Updates one object's authored transform. */
bool SceneDocument::set_transform(core::ForgeUUID id, core::Transform transform) {
    if (!transform.is_finite()) {
        return false;
    }
    ForgeObject* const object = find(id);
    if (object == nullptr) {
        return false;
    }
    object->transform = transform;
    return true;
}

/** Changes one object's parent. Cycles are rejected locally. */
bool SceneDocument::reparent(core::ForgeUUID id, core::ForgeUUID parent) {
    ForgeObject* const object = find(id);
    if (object == nullptr || id == parent) {
        return false;
    }
    if (!parent.is_nil() && !contains(parent)) {
        return false;
    }
    for (core::ForgeUUID current = parent; !current.is_nil();) {
        const ForgeObject* const ancestor = find(current);
        if (ancestor == nullptr) {
            return false;
        }
        if (ancestor->parent == id) {
            return false;
        }
        current = ancestor->parent;
    }
    object->parent = parent;
    return true;
}

/** @return Mutable object by UUID, or null when absent. */
ForgeObject* SceneDocument::find(core::ForgeUUID id) noexcept {
    const auto found = std::find_if(objects_.begin(), objects_.end(), [id](const ForgeObject& object) {
        return same_id(object, id);
    });
    return found == objects_.end() ? nullptr : &*found;
}

/** @return Object by UUID, or null when absent. */
const ForgeObject* SceneDocument::find(core::ForgeUUID id) const noexcept {
    const auto found = std::find_if(objects_.begin(), objects_.end(), [id](const ForgeObject& object) {
        return same_id(object, id);
    });
    return found == objects_.end() ? nullptr : &*found;
}

/** @return True when the scene owns this logical object ID. */
bool SceneDocument::contains(core::ForgeUUID id) const noexcept {
    return find(id) != nullptr;
}

/** @return Logical scene objects in hierarchy storage order. */
std::span<const ForgeObject> SceneDocument::objects() const noexcept {
    return objects_;
}

/** @return Logical scene object count. */
std::size_t SceneDocument::count() const noexcept {
    return objects_.size();
}

} // namespace sunrise::izanami::project::scene