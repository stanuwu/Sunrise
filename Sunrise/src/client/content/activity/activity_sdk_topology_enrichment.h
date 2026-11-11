#pragma once

#include <cstdint>
#include <vector>

#include "activity_sdk_topology_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::topology_enrichment {

/** One final slot row before string offsets and capability ranges are linked. */
struct Slot final {
    topology_inventory::Text name{};
    topology_inventory::Text senseSchemaId{};
    topology_inventory::Text authSchemaId{};
    std::uint32_t componentClass{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t senseSchema{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t authSchema{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t flags{};
    std::uint32_t dialogueCueCount{};
    /** Exact authored output count for a resolved type-24 target. */
    std::uint32_t outputChannelCount{};
    state::activity_sdk::format::Range aliases{};
};

/** Resolved package-inline names and descriptor/reflection joins in topology row order. */
struct Snapshot final {
    std::vector<topology_inventory::Text> bubbleNames{};
    std::vector<Slot> slots{};
    std::vector<topology_inventory::Text> slotAliases{};
};

/** Recomputes every name and schema join and rejects output drift. */
[[nodiscard]] bool validate(const topology_inventory::Snapshot& topology,
                            const Snapshot& enrichment) noexcept;

/** Resolves exact inline names and package descriptor candidates transactionally. */
[[nodiscard]] bool build(const topology_inventory::Snapshot& topology, Snapshot& output) noexcept;

/** Resolves generated names and package identities without a second output validation pass. */
[[nodiscard]] bool build_generated(const topology_inventory::Snapshot& topology,
                                   Snapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::topology_enrichment
