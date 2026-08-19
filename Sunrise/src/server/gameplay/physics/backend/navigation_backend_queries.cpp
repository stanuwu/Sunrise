#include "navigation_backend.h"
#include "navigation_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend {

using namespace navigation_internal;

/**
 * Keeps one point only when its exact agent pose is collision-free.
 * @param query Exact scene, bubble, point, and agent input.
 * @param projected Receives the unchanged safe point on success.
 * @return Explicit validation, context, bounds, or collision status.
 */
NavigationStatus NavigationBackend::project_point(const ProjectPointQuery& query,
                                                  NavigationPoint& projected) const noexcept {
    if (!initialized_ || physics_ == nullptr || query.point.scene.slot >= scenes_.size()) {
        return NavigationStatus::staleScene;
    }
    const SceneSlot& scene = scenes_[query.point.scene.slot];
    if (!scene.occupied || scene.generation != query.point.scene.generation) {
        return NavigationStatus::staleScene;
    }
    if (query.point.bubble != scene.spec.bubble || query.agent.gate.bubble != scene.spec.bubble) {
        return NavigationStatus::contextMismatch;
    }
    if (query.agent.stableOwnerId == 0 || query.agent.shapeId == 0
        || query.agent.gate.layer >= kCollisionLayerCount || !finite(query.point.position)
        || !valid(query.agent.shape)) {
        return NavigationStatus::invalidQuery;
    }
    const float radius = conservative_radius(query.agent.shape);
    if (!finite(radius) || !inside(scene.bounds, query.point.position, radius)) {
        return NavigationStatus::outsideBounds;
    }
    SceneState physicsScene{};
    if (!physics_->read_scene(scene.spec.physicsScene, physicsScene)) {
        return NavigationStatus::staleScene;
    }

    OverlapQuery overlap{};
    overlap.scene = scene.spec.physicsScene;
    overlap.stableOwnerId = query.agent.stableOwnerId;
    overlap.shapeId = query.agent.shapeId;
    overlap.gate = query.agent.gate;
    overlap.shape = query.agent.shape;
    overlap.transform.position = query.point.position;
    overlap.ignoreOwnerId = query.agent.ignoreOwnerId;
    overlap.includeTriggers = false;
    QueryHitBuffer hits{};
    const QueryStatus queryStatus = physics_->overlap(overlap, hits);
    if (queryStatus != QueryStatus::success) {
        return map_status(queryStatus);
    }
    if (hits.count != 0) {
        return NavigationStatus::blocked;
    }
    projected = query.point;
    return NavigationStatus::success;
}

/**
 * Tests one exact-scene segment with a conservative agent sweep.
 * @param query Start, goal, and agent input.
 * @return Success only when both endpoints and the segment are clear.
 */
NavigationStatus NavigationBackend::reachable(const ReachabilityQuery& query) const noexcept {
    if (query.start.scene != query.goal.scene || query.start.bubble != query.goal.bubble) {
        return NavigationStatus::contextMismatch;
    }
    ProjectPointQuery startQuery{};
    startQuery.point = query.start;
    startQuery.agent = query.agent;
    NavigationPoint start{};
    const NavigationStatus startStatus = project_point(startQuery, start);
    if (startStatus != NavigationStatus::success) {
        return startStatus;
    }

    ProjectPointQuery goalQuery{};
    goalQuery.point = query.goal;
    goalQuery.agent = query.agent;
    NavigationPoint goal{};
    const NavigationStatus goalStatus = project_point(goalQuery, goal);
    if (goalStatus != NavigationStatus::success) {
        return goalStatus;
    }
    if (same_position(start.position, goal.position)) {
        return NavigationStatus::success;
    }

    const SceneSlot& scene = scenes_[query.start.scene.slot];
    SweepQuery sweep{};
    sweep.scene = scene.spec.physicsScene;
    sweep.stableOwnerId = query.agent.stableOwnerId;
    sweep.shapeId = query.agent.shapeId;
    sweep.gate = query.agent.gate;
    sweep.shape = query.agent.shape;
    sweep.transform.position = start.position;
    sweep.displacement = {goal.position.x - start.position.x,
                          goal.position.y - start.position.y,
                          goal.position.z - start.position.z};
    sweep.ignoreOwnerId = query.agent.ignoreOwnerId;
    sweep.includeTriggers = false;
    QueryHitBuffer hits{};
    const QueryStatus queryStatus = physics_->sweep(sweep, hits);
    if (queryStatus != QueryStatus::success) {
        return map_status(queryStatus);
    }
    return hits.count == 0 ? NavigationStatus::success : NavigationStatus::blocked;
}

/**
 * Returns only the safe one- or two-point scriptless path.
 * @param query Start, goal, and agent input.
 * @param path Receives a path only on success.
 * @return Explicit reachability failure or success.
 */
NavigationStatus NavigationBackend::find_path(const ReachabilityQuery& query,
                                              NavigationPath& path) const noexcept {
    const NavigationStatus status = reachable(query);
    if (status != NavigationStatus::success) {
        return status;
    }
    NavigationPath next{};
    next.points[0] = query.start;
    next.count = 1;
    if (!same_position(query.start.position, query.goal.position)) {
        next.points[1] = query.goal;
        next.count = 2;
    }
    path = next;
    return NavigationStatus::success;
}

} // namespace sunrise::server::gameplay::physics::backend
