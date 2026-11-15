#pragma once

#include <cstdint>

#include "../../state/activity_sdk/runtime.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_scriptables {

/** Exact object and slot identity needed to join one live state-local SDK slot. */
struct SourceIdentity final {
    std::uint32_t scenarioTag{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t authSchema{};
    std::uint32_t objectRow{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t stateRow{state::activity_sdk::format::kAbsentIndex};
    std::uint16_t slotIndex{};
    std::uint8_t slotType{};
};

/** Exact generated group and state selected for one SDK slot. */
struct Resolution final {
    host::ScriptableTarget target{};
    state::build_data::scenarios::RosterGroup rosterGroup{};
    std::uint32_t scenarioRow{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t stateRow{state::activity_sdk::format::kAbsentIndex};
    std::int32_t region{-1};
};

/** Result of joining one package source to the generated SDK. */
enum class Status : std::uint8_t {
    ready,
    invalidView,
    invalidSource,
    missingSource,
    ambiguousSource,
};

/**
 * Resolves one exact occurrence and stages its complete group in one region.
 * @param region The client's region, or the occurrence's own in a bubble the client is not in.
 */
[[nodiscard]] Status resolve(const state::activity_sdk::BoundView& view,
                             const SourceIdentity& source,
                             std::int32_t region,
                             Resolution& output) noexcept;

} // namespace sunrise::server::activity::activity_sdk_scriptables
