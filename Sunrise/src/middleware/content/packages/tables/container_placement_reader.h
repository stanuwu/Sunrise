#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_808091.h"
#include "../schema/generated/tag_classes_808094.h"
#include "../schema/generated/tag_classes_80809c.h"
#include "authored_placement_reader.h"

namespace sunrise::middleware::content::packages::tables {

/** Installed class of a placed object definition. */
inline constexpr std::uint32_t kPlacedClassDefinitionClass = schema::generated::SEntity::kClass;
/** Its ordered construction rows use this element class and fixed stride. */
inline constexpr std::uint32_t kPlacedClassBuildRowClass =
    schema::generated::SPatternComponent::kClass;
inline constexpr std::size_t kPlacedClassBuildDescriptor =
    offsetof(schema::generated::SEntity, components);
inline constexpr std::size_t kPlacedClassBuildStride = sizeof(schema::generated::SPatternComponent);
/** The class definition records its authored type byte here. */
inline constexpr std::size_t kPlacedClassObjectTypeOffset =
    offsetof(schema::generated::SEntity, objectType);
/** Flag byte after the type; bit 0x10 is what the game's replication predicate reads. */
inline constexpr std::size_t kPlacedClassFlagsOffset = offsetof(schema::generated::SEntity, flags);
inline constexpr std::uint8_t kPlacedClassNetworkReplicatedBit = 0x10;
/** Installed class of one placed object config. */
inline constexpr std::uint32_t kPlacedConfigClass = schema::generated::SEntityConfig::kClass;
/** Its component rows use this element class and four-word layout. */
inline constexpr std::uint32_t kPlacedConfigComponentRowClass =
    schema::generated::PlacedConfigComponentRow::kClass;
inline constexpr std::size_t kPlacedConfigComponentDescriptor =
    offsetof(schema::generated::SEntityConfig, componentLinks);
inline constexpr std::size_t kPlacedConfigComponentStride =
    sizeof(schema::generated::PlacedConfigComponentRow);
/** Installed class of an object-behavior root. */
inline constexpr std::uint32_t kObjectBehaviorRootClass =
    schema::generated::ObjectBehaviorRoot::kClass;

/** Parsed header fields of one placed class definition. */
struct PlacedClassDefinition final {
    Array builds{};
    std::uint8_t objectType{};
    /** True when objects of this class replicate over the network. */
    bool networkReplicated{};
};

/** One ordered class-construction row. */
struct PlacedClassBuildRow final {
    std::uint32_t configTag{};
    std::uint32_t secondWord{};
    std::uint32_t thirdWord{};
};

/** One exact four-word config component row. */
struct PlacedConfigComponentRow final {
    std::uint32_t firstWord{};
    std::uint32_t secondWord{};
    std::uint32_t componentClass{};
    std::uint32_t fourthWord{};
};

/** @return The established label for a binary-carried object-type value. */
[[nodiscard]] const char* placed_object_type_name(std::uint8_t type) noexcept;

/** Reads the exact uniform-scale lane without changing the legacy authored-placement record. */
[[nodiscard]] bool container_placement_uniform_scale_at(std::span<const std::byte> blob,
                                                        const Array& placements,
                                                        std::size_t index,
                                                        float& output) noexcept;

/** Reads the typed build array and authored type of one exact class definition. */
[[nodiscard]] bool placed_class_definition(std::span<const std::byte> blob,
                                           PlacedClassDefinition& output) noexcept;

/** Reads one ordered build row from a validated placed class definition. */
[[nodiscard]] bool placed_class_build_at(std::span<const std::byte> blob,
                                         const Array& builds,
                                         std::size_t index,
                                         PlacedClassBuildRow& output) noexcept;

/** Reads the typed component array from one exact placed config. */
[[nodiscard]] bool placed_config_components(std::span<const std::byte> blob,
                                            Array& output) noexcept;

/** Reads one four-word row from a validated config component array. */
[[nodiscard]] bool placed_config_component_at(std::span<const std::byte> blob,
                                              const Array& components,
                                              std::size_t index,
                                              PlacedConfigComponentRow& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
