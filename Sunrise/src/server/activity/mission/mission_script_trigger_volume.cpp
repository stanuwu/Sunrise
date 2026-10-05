#include "mission_script_trigger_volume.h"

#include <algorithm>
#include <cmath>

namespace sunrise::server::activity::mission::trigger_volume {

namespace catalog = state::build_data::scriptables;

/** Tests the footprint in plan, then the height against that triangle's interpolated floor. */
bool contains(const catalog::Snapshot& world,
              const catalog::TriggerVolumeInstance& volume,
              const std::array<float, 3>& point) noexcept {
    if (!volume.complete || volume.active == 0 || !std::isfinite(volume.extrusion)
        || volume.extrusion < 0.0F || volume.firstVertex > world.triggerVolumeVertices.size()
        || volume.vertexCount > world.triggerVolumeVertices.size() - volume.firstVertex
        || volume.firstTriangle > world.triggerVolumeTriangles.size()
        || volume.triangleCount > world.triggerVolumeTriangles.size() - volume.firstTriangle) {
        return false;
    }
    for (std::size_t row = volume.firstTriangle; row < volume.firstTriangle + volume.triangleCount;
         ++row) {
        const auto& indices = world.triggerVolumeTriangles[row].indices;
        if (std::any_of(indices.begin(), indices.end(), [&volume](std::uint8_t index) {
                return index >= volume.vertexCount;
            })) {
            return false;
        }
        const auto& a = world.triggerVolumeVertices[volume.firstVertex + indices[0]].value;
        const auto& b = world.triggerVolumeVertices[volume.firstVertex + indices[1]].value;
        const auto& c = world.triggerVolumeVertices[volume.firstVertex + indices[2]].value;
        // Barycentric weights of the point's footprint in the triangle's plan projection.
        const double denominator = static_cast<double>(b[1] - c[1]) * (a[0] - c[0])
                                   + static_cast<double>(c[0] - b[0]) * (a[1] - c[1]);
        if (denominator == 0.0) {
            continue;
        }
        const double first = (static_cast<double>(b[1] - c[1]) * (point[0] - c[0])
                              + static_cast<double>(c[0] - b[0]) * (point[1] - c[1]))
                             / denominator;
        const double second = (static_cast<double>(c[1] - a[1]) * (point[0] - c[0])
                               + static_cast<double>(a[0] - c[0]) * (point[1] - c[1]))
                              / denominator;
        const double third = 1.0 - first - second;
        if (first < 0.0 || second < 0.0 || third < 0.0) {
            continue;
        }
        const double floor = first * a[2] + second * b[2] + third * c[2];
        return point[2] >= floor && point[2] <= floor + volume.extrusion;
    }
    return false;
}

} // namespace sunrise::server::activity::mission::trigger_volume
