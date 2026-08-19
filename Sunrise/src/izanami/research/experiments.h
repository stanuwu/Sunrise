#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::izanami::research {

enum class ExperimentId : std::uint8_t {
    residentEntitySpawn,
    entityTransformLifecycle,
    crosshairRaycastPosition,
    cleanEntityDestruction,
    stockFamilyMapNodePatch,
    liveObjectCorrelation,
    count,
};

struct ExperimentRecord {
    ExperimentId id{ExperimentId::count};
    std::string_view name{};
    std::string_view requiredCapability{};
};

[[nodiscard]] std::span<const ExperimentRecord> experiment_records() noexcept;

} // namespace sunrise::izanami::research