#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::izanami::research {

enum class EvidenceStatus : std::uint8_t {
    unknown,
    historicalLead,
    openPrLead,
    mergedVerified,
    currentBuildVerified,
};

enum class NativeTargetId : std::uint8_t {
    packageHeaderValidator,
    loadedEntityPlacementInitialize,
    loadedEntityDirectInitialize,
    objectFactory,
    objectTransform,
    worldRaycast,
    objectDatumDescriptor,
    mapNodeConsumer,
    liveObjectRegistry,
    havokSimulationStep,
    destinyDirectorMenuNode,
    count,
};

struct NativeTargetRecord {
    NativeTargetId id{NativeTargetId::count};
    EvidenceStatus status{EvidenceStatus::unknown};
    std::string_view capability{};
    std::string_view notes{};
};

[[nodiscard]] std::span<const NativeTargetRecord> native_target_records() noexcept;

} // namespace sunrise::izanami::research
