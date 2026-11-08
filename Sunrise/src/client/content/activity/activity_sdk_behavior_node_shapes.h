#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../middleware/content/packages/schema/generated/tag_classes_808029.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80804d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80804e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808093.h"

namespace sunrise::client::content::activity::sdk_generation::behavior_inventory::node_shapes {

namespace generated = middleware::content::packages::schema::generated;

/** One behavior node layout the program walk reads. */
struct NodeShape final {
    std::uint32_t classId{};
    std::uint16_t size{};
    std::array<std::uint8_t, 3> roots{};
    std::uint8_t rootCount{};
    std::uint8_t expressionOffset{};
};

// Class id, size, embedded root offsets and expression offset of each node layout.
inline constexpr std::array kNodeShapes{
    NodeShape{generated::BehaviorTimer::kClass,
              sizeof(generated::BehaviorTimer),
              {},
              0,
              offsetof(generated::BehaviorTimer, h10b2432c)},
    NodeShape{generated::BehaviorTarget::kClass, sizeof(generated::BehaviorTarget)},
    NodeShape{generated::BehaviorSpawnProjectile::kClass,
              sizeof(generated::BehaviorSpawnProjectile)},
    NodeShape{generated::BehaviorSpawn::kClass, sizeof(generated::BehaviorSpawn)},
    NodeShape{generated::BehaviorSearch::kClass,
              sizeof(generated::BehaviorSearch),
              {offsetof(generated::BehaviorSearch, behaviors)},
              1},
    NodeShape{generated::BehaviorResource::kClass, sizeof(generated::BehaviorResource)},
    NodeShape{generated::BehaviorRemoveHopOn::kClass, sizeof(generated::BehaviorRemoveHopOn)},
    NodeShape{generated::BehaviorRedirect::kClass,
              sizeof(generated::BehaviorRedirect),
              {offsetof(generated::BehaviorRedirect, behaviors)},
              1},
    NodeShape{generated::BehaviorPlayerInteraction::kClass,
              sizeof(generated::BehaviorPlayerInteraction)},
    NodeShape{generated::BehaviorNavpoint::kClass, sizeof(generated::BehaviorNavpoint)},
    NodeShape{generated::BehaviorInvestmentUnlockFlag::kClass,
              sizeof(generated::BehaviorInvestmentUnlockFlag),
              {offsetof(generated::BehaviorInvestmentUnlockFlag, hE0b11eea),
               offsetof(generated::BehaviorInvestmentUnlockFlag, h77f33325)},
              2},
    NodeShape{generated::BehaviorKill::kClass, sizeof(generated::BehaviorKill)},
    NodeShape{generated::BehaviorIncident::kClass, sizeof(generated::BehaviorIncident)},
    NodeShape{generated::BehaviorGlobal::kClass,
              sizeof(generated::BehaviorGlobal),
              {offsetof(generated::BehaviorGlobal, behaviors)},
              1},
    NodeShape{generated::BehaviorFilter::kClass,
              sizeof(generated::BehaviorFilter),
              {offsetof(generated::BehaviorFilter, hA5f38d62),
               offsetof(generated::BehaviorFilter, h549767fd)},
              2},
    NodeShape{generated::BehaviorExternalBehavior::kClass,
              sizeof(generated::BehaviorExternalBehavior)},
    NodeShape{generated::BehaviorDefer::kClass,
              sizeof(generated::BehaviorDefer),
              {offsetof(generated::BehaviorDefer, behaviors)},
              1},
    NodeShape{generated::BehaviorCounter::kClass,
              sizeof(generated::BehaviorCounter),
              {},
              0,
              offsetof(generated::BehaviorCounter, expression)},
    NodeShape{generated::BehaviorAddHopOn::kClass, sizeof(generated::BehaviorAddHopOn)},
    NodeShape{generated::BehaviorExitSeat::kClass, sizeof(generated::BehaviorExitSeat)},
    NodeShape{generated::BehaviorStorageChannel::kClass,
              sizeof(generated::BehaviorStorageChannel),
              {},
              0,
              offsetof(generated::BehaviorStorageChannel, expression)},
    NodeShape{generated::BehaviorSpawnRandom::kClass, sizeof(generated::BehaviorSpawnRandom)},
    NodeShape{generated::BehaviorChannelComparison::kClass,
              sizeof(generated::BehaviorChannelComparison)},
    NodeShape{generated::BehaviorConditionStructure80804D5E::kClass,
              sizeof(generated::BehaviorConditionStructure80804D5E)},
    NodeShape{generated::BehaviorConditionLeaf80804D71::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D71)},
    NodeShape{generated::BehaviorConditionLeaf80804D72::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D72)},
    NodeShape{generated::BehaviorConditionStructure80804D73::kClass,
              sizeof(generated::BehaviorConditionStructure80804D73)},
    NodeShape{generated::BehaviorConditionLeaf80804D74::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D74)},
    NodeShape{generated::BehaviorConditionLeaf80804D75::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D75)},
    NodeShape{generated::BehaviorConditionLeaf80804D76::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D76)},
    NodeShape{generated::BehaviorConditionStructure80804D78::kClass,
              sizeof(generated::BehaviorConditionStructure80804D78)},
    NodeShape{generated::BehaviorConditionLeaf80804D7A::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D7A)},
    NodeShape{generated::BehaviorConditionLeaf80804D7B::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D7B)},
    NodeShape{generated::BehaviorConditionLeaf80804D7C::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D7C)},
    NodeShape{generated::BehaviorConditionLeaf80804D80::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D80)},
    NodeShape{generated::BehaviorConditionLeaf80804D81::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D81)},
    NodeShape{generated::BehaviorConditionLeaf80804D82::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D82)},
    NodeShape{generated::BehaviorConditionLeaf80804D83::kClass,
              sizeof(generated::BehaviorConditionLeaf80804D83)},
    NodeShape{generated::BehaviorConditionGroup::kClass, sizeof(generated::BehaviorConditionGroup)},
    NodeShape{generated::BehaviorTownPresence::kClass, sizeof(generated::BehaviorTownPresence)},
    NodeShape{generated::BehaviorResourceBehavior::kClass,
              sizeof(generated::BehaviorResourceBehavior),
              {offsetof(generated::BehaviorResourceBehavior, behaviors)},
              1},
    NodeShape{generated::BehaviorResourceLeaf80804E13::kClass,
              sizeof(generated::BehaviorResourceLeaf80804E13)},
    NodeShape{generated::BehaviorResourceLeaf80804E14::kClass,
              sizeof(generated::BehaviorResourceLeaf80804E14)},
    NodeShape{generated::BehaviorResourceLeaf80804E15::kClass,
              sizeof(generated::BehaviorResourceLeaf80804E15)},
    NodeShape{generated::BehaviorSelect::kClass,
              sizeof(generated::BehaviorSelect),
              {offsetof(generated::BehaviorSelect, h8edfa376),
               offsetof(generated::BehaviorSelect, h46040d77),
               offsetof(generated::BehaviorSelect, h79a8f9b3)},
              3},
    NodeShape{generated::BehaviorRelativeTeleportation::kClass,
              sizeof(generated::BehaviorRelativeTeleportation),
              {},
              0,
              offsetof(generated::BehaviorRelativeTeleportation, h762b49fd)},
    NodeShape{generated::BehaviorChannelLink::kClass, sizeof(generated::BehaviorChannelLink)},
};

/** @return The layout of one node class, or null when the walk does not read it. */
[[nodiscard]] inline const NodeShape* shape(std::uint32_t classId) noexcept {
    const auto found = std::find_if(kNodeShapes.begin(),
                                    kNodeShapes.end(),
                                    [classId](const auto& row) { return row.classId == classId; });
    return found == kNodeShapes.end() ? nullptr : &*found;
}

} // namespace sunrise::client::content::activity::sdk_generation::behavior_inventory::node_shapes
