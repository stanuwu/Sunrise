#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>

#include "../schema/generated/tag_classes_80807e.h"
#include "../schema/generated/tag_classes_808083.h"
#include "../schema/generated/tag_classes_808094.h"
#include "../schema/generated/tag_classes_808098.h"
#include "../schema/generated/tag_classes_808099.h"
#include "../schema/generated/tag_classes_80809a.h"
#include "../schema/generated/tag_classes_80809c.h"
#include "authored_placement_reader.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Config fields holding the two self-relative component pointers. */
inline constexpr std::size_t kConfigPrimaryComponentPointerOffset =
    offsetof(schema::generated::SEntityConfig, defaultInstance);
inline constexpr std::size_t kConfigSecondaryComponentPointerOffset =
    offsetof(schema::generated::SEntityConfig, definition);

/** Exact component classes and slot type of an authored type-1 squad spawner. */
inline constexpr std::uint32_t kAuthoredSquadSpawnerPrimaryClass =
    schema::generated::SSquadComponent::kClass;
inline constexpr std::uint32_t kAuthoredSquadSpawnerSecondaryClass =
    schema::generated::SSquadComponentPeer::kClass;
inline constexpr std::uint16_t kAuthoredSquadSpawnerSlotType = 1;
/** Spawner-secondary fields for its two raw object-slot refs and member array. */
inline constexpr std::size_t kAuthoredSquadSpawnerRawReference98Offset =
    offsetof(schema::generated::SSquadComponentPeer, h81eeb835.ruleSlotAObjectKey);
inline constexpr std::size_t kAuthoredSquadSpawnerRawReferenceA0Offset =
    offsetof(schema::generated::SSquadComponentPeer, h81eeb835.ruleSlotBObjectKey);
inline constexpr std::size_t kAuthoredSquadSpawnerMemberArrayOffset =
    offsetof(schema::generated::SSquadComponentPeer, h81eeb835.cells);
/** Each raw ref is one u64 over the slot key, type and index. */
static_assert(kAuthoredSquadSpawnerRawReferenceA0Offset
              == kAuthoredSquadSpawnerRawReference98Offset + sizeof(std::uint64_t));
static_assert(kAuthoredSquadSpawnerMemberArrayOffset
              == kAuthoredSquadSpawnerRawReferenceA0Offset + sizeof(std::uint64_t));
/** Exact member array element class and stride. */
inline constexpr std::uint32_t kAuthoredSquadMemberClass = schema::generated::SquadMember::kClass;
inline constexpr std::size_t kAuthoredSquadMemberStride = sizeof(schema::generated::SquadMember);
/** Six consecutive candidate-array descriptors begin after the member key and reserved word. */
inline constexpr std::size_t kAuthoredSquadVariantCount =
    std::tuple_size_v<decltype(schema::generated::C8080467d::candidateVariants)>;
inline constexpr std::size_t kAuthoredSquadFirstCandidateArrayOffset =
    offsetof(schema::generated::SquadMember, characterChoices.candidateVariants);
inline constexpr std::size_t kAuthoredSquadCandidateArrayStride =
    sizeof(schema::generated::G8080467d0);
/** Exact candidate array element class and stride. */
inline constexpr std::uint32_t kAuthoredSquadCandidateClass =
    schema::generated::SquadCandidate::kClass;
inline constexpr std::size_t kAuthoredSquadCandidateStride =
    sizeof(schema::generated::SquadCandidate);

/** Exact component classes and slot type of an authored type-66 spawn rule. */
inline constexpr std::uint32_t kAuthoredSquadRulePrimaryClass =
    schema::generated::SSquadRuleComponent::kClass;
inline constexpr std::uint32_t kAuthoredSquadRuleSecondaryClass =
    schema::generated::SSquadRuleComponentPeer::kClass;
inline constexpr std::uint16_t kAuthoredSquadRuleSlotType = 66;
/** Rule-primary field and exact type of its authored point array. */
inline constexpr std::size_t kAuthoredSquadRulePointArrayOffset =
    offsetof(schema::generated::SSquadRuleComponent, hE6533acf.points);
inline constexpr std::uint32_t kAuthoredSquadRulePointClass =
    schema::generated::SquadRulePoint::kClass;
inline constexpr std::size_t kAuthoredSquadRulePointStride =
    sizeof(schema::generated::SquadRulePoint);

/**
 * A spawner with both raw refs absent may carry its own point set. The primary component points
 * at a point-set component, the secondary at its placement peer. The pair is reciprocal.
 */
inline constexpr std::uint32_t kAuthoredSquadInlinePointSetClass =
    schema::generated::SquadInlinePointSet::kClass;
inline constexpr std::uint32_t kAuthoredSquadInlinePlacementClass =
    schema::generated::SquadInlinePlacement::kClass;
inline constexpr std::size_t kAuthoredSquadSpawnerInlinePointSetOffset =
    offsetof(schema::generated::SSquadComponent, h81eeb835.inlinePointSet);
inline constexpr std::size_t kAuthoredSquadSpawnerInlinePlacementOffset =
    offsetof(schema::generated::SSquadComponentPeer, h81eeb835.inlinePlacement);
/** Point-set fields: the typed point array and the initial point index. */
inline constexpr std::size_t kAuthoredSquadInlinePointArrayOffset =
    offsetof(schema::generated::SquadInlinePointSet, hE6533acf.points);
inline constexpr std::size_t kAuthoredSquadInlineInitialPointOffset =
    offsetof(schema::generated::SquadInlinePointSet, hE6533acf.initialPoint);
/** The placement record sits inside the peer component and has no class marker before it. */
inline constexpr std::size_t kAuthoredSquadInlinePlacementRecordOffset =
    offsetof(schema::generated::SquadInlinePlacement, placementRecord);

/** All bits set marks a candidate with no placement identity. */
inline constexpr std::uint64_t kAbsentAuthoredSquadPlacementIdentity = 0xFFFFFFFFFFFFFFFFULL;

/** The two config components after their owner, class, peer class, and peer offset agree. */
struct ComponentPair final {
    std::size_t primaryOffset{};
    std::size_t secondaryOffset{};
    std::uint32_t primaryClass{};
    std::uint32_t secondaryClass{};
};

/** Exact package tables owned by one authored type-1 spawner config. */
struct AuthoredSquadSpawner final {
    ComponentPair components{};
    std::uint64_t rawReference98{};
    std::uint64_t rawReferenceA0{};
    Array members{};
};

/** One fixed-stride member and its six exact typed candidate arrays. */
struct AuthoredSquadMember final {
    std::uint32_t key{};
    std::uint32_t reserved{};
    std::array<Array, kAuthoredSquadVariantCount> candidates{};
};

/** One candidate whose placed-entry pointer and complete finite transform validated. */
struct AuthoredSquadCandidate final {
    std::size_t descriptorOffset{};
    std::int64_t placementRelative{};
    std::size_t placementOffset{};
    std::array<std::byte, 16> descriptorTail{};
    bool hasPlacement{};
    std::uint32_t placementClass{};
    std::uint32_t classDefinitionTag{};
    std::array<float, 4> rotation{};
    std::array<std::uint32_t, 4> rotationBits{};
    std::array<float, 3> position{};
    std::array<std::uint32_t, 3> positionBits{};
    float uniformScale{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::uint64_t placementIdentity{};
};

/** One spawner's own point set and the placement its single point names. */
struct AuthoredSquadInlinePointSet final {
    std::size_t pointSetOffset{};
    std::size_t placementComponentOffset{};
    std::size_t placementOffset{};
    Array points{};
    std::uint32_t initialPointIndex{};
    /** The placement fields. Descriptor and pointer fields stay zero. */
    AuthoredSquadCandidate placement{};
};

/** Exact package table owned by one authored type-66 spawn-rule config. */
struct AuthoredSquadRule final {
    ComponentPair components{};
    Array points{};
};

/** One fixed-stride authored rule point. Its remaining bytes stay opaque. */
struct AuthoredSquadRulePoint final {
    std::size_t rowOffset{};
    std::uint64_t placementIdentity{};
    std::array<std::byte, 64> rawTail{};
};

/** Reads the config's two reciprocal component endpoints without classifying the pair. */
[[nodiscard]] bool config_component_pair(std::span<const std::byte> blob,
                                         std::uint32_t configTag,
                                         ComponentPair& output) noexcept;

/** Reads the exact type-1 spawner pair, raw references, and bounded member array. */
[[nodiscard]] bool authored_squad_spawner(std::span<const std::byte> blob,
                                          std::uint32_t configTag,
                                          AuthoredSquadSpawner& output) noexcept;

/** Reads one member and validates all six typed candidate-array descriptors. */
[[nodiscard]] bool authored_squad_member_at(std::span<const std::byte> blob,
                                            const AuthoredSquadSpawner& spawner,
                                            std::uint64_t index,
                                            AuthoredSquadMember& output) noexcept;

/**
 * Reads one usable candidate. Null targets, wrong placement markers, non-finite transforms, and
 * zero/all-one placement identities are refused.
 */
[[nodiscard]] bool authored_squad_candidate_at(std::span<const std::byte> blob,
                                               const AuthoredSquadMember& member,
                                               std::size_t variant,
                                               std::uint64_t index,
                                               AuthoredSquadCandidate& output) noexcept;

/**
 * Reads the complete SDK candidate record. Unlike the runnable-candidate helper above, this
 * preserves null targets and zero/all-one authored identities exactly.
 */
[[nodiscard]] bool authored_squad_candidate_record_at(std::span<const std::byte> blob,
                                                      const AuthoredSquadMember& member,
                                                      std::size_t variant,
                                                      std::uint64_t index,
                                                      AuthoredSquadCandidate& output) noexcept;

/**
 * Reads a spawner's own point set. `present` is false when the spawner has none. A present but
 * malformed pair fails.
 */
[[nodiscard]] bool authored_squad_inline_point_set(std::span<const std::byte> blob,
                                                   std::uint32_t configTag,
                                                   const AuthoredSquadSpawner& spawner,
                                                   bool& present,
                                                   AuthoredSquadInlinePointSet& output) noexcept;

/** Reads the exact type-66 rule pair and bounded typed point array. */
[[nodiscard]] bool authored_squad_rule(std::span<const std::byte> blob,
                                       std::uint32_t configTag,
                                       AuthoredSquadRule& output) noexcept;

/** Reads one identity from a validated authored rule point. */
[[nodiscard]] bool authored_squad_rule_point_at(std::span<const std::byte> blob,
                                                const AuthoredSquadRule& rule,
                                                std::uint64_t index,
                                                AuthoredSquadRulePoint& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
