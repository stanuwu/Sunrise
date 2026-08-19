#include "experiments.h"

namespace sunrise::izanami::research {

namespace {

constexpr std::array<ExperimentRecord, static_cast<std::size_t>(ExperimentId::count)>
    kExperiments{{
        {ExperimentId::residentEntitySpawn, "resident entity spawn", "world.entity.spawn.loaded"},
        {ExperimentId::entityTransformLifecycle,
         "entity transform lifecycle",
         "world.transform.write"},
        {ExperimentId::crosshairRaycastPosition,
         "crosshair raycast position",
         "physics.raycast.position"},
        {ExperimentId::cleanEntityDestruction, "clean entity destruction", "world.destroy"},
        {ExperimentId::stockFamilyMapNodePatch,
         "stock-family map-node patch",
         "package.accept.patch"},
        {ExperimentId::liveObjectCorrelation,
         "live object correlation",
         "diagnostics.native_object_explorer"},
    }};

} // namespace

/** @return Ordered native-validation experiments from the onboarding handoff. */
std::span<const ExperimentRecord> experiment_records() noexcept {
    return kExperiments;
}

} // namespace sunrise::izanami::research