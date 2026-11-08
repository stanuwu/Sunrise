#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_808076.h"

namespace sunrise::middleware::content::packages::tables {

/** Installed activity-definition table tag. */
inline constexpr std::uint32_t kActivityDefinitionTableTag = 0x81327CF0U;
/** Installed activity-definition table class. */
inline constexpr std::uint32_t kActivityDefinitionTableClass =
    schema::generated::ActivityDefinitionTable::kClass;
/** The installed table holds this many definitions; a mod only appends rows after them. */
inline constexpr std::size_t kStockActivityDefinitionCount = 1170;
/** 12 biased wire bits name activity indices 0 to 4094, so a table holds at most 4095 rows. */
inline constexpr std::size_t kActivityDefinitionMaximumCount = 4095;
/** An internal name must terminate within this many bytes. */
inline constexpr std::size_t kActivityDefinitionInternalNameCapacity = 256;
/** The all-one tag marks an activity with no matchmaking configuration. */
inline constexpr std::uint32_t kActivityDefinitionNoMatchmakingConfig = 0xFFFFFFFFU;

/** One indexed activity definition and the fields needed by the SDK inventory. */
struct ActivityDefinition final {
    std::uint32_t activityIndex{};
    std::uint32_t definitionHash{};
    std::size_t recordOffset{};
    std::size_t recordLength{};
    std::size_t matchmakingPointerOffset{};
    std::uint32_t matchmakingConfigTag{kActivityDefinitionNoMatchmakingConfig};
    bool hasMatchmakingConfig{};
    std::size_t internalNamePointerOffset{};
    std::array<char, kActivityDefinitionInternalNameCapacity> internalName{};
    std::uint16_t internalNameLength{};
    std::int32_t requiredLevel{};
    std::int32_t requiredPower{};
    std::int32_t requiredLevel2{};
    std::int32_t requiredPower2{};
    std::uint16_t unlockSlot{};
    std::uint8_t typeIndex{};
    std::uint32_t gameplaySettingsHash{};
    std::uint8_t destinationIndex{};
};

/** Visitor called once per activity definition in activity-index order. */
using ActivityDefinitionVisitor = bool (*)(void* context,
                                           const ActivityDefinition& definition) noexcept;

/**
 * Validates and visits the complete activity-definition table, stock rows and mod rows alike.
 * No visitor call occurs unless every definition is valid.
 * @return True when the table is valid and the visitor accepts every definition.
 */
[[nodiscard]] bool visit_activity_definitions(std::span<const std::byte> blob,
                                              ActivityDefinitionVisitor visitor,
                                              void* context) noexcept;

} // namespace sunrise::middleware::content::packages::tables
