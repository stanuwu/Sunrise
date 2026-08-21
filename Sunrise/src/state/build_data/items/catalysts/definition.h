#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::state::build_data::items::catalysts {

/** The target build contains fewer than 128 exotic weapon catalyst-shaped sockets. */
inline constexpr std::size_t kDefinitionCapacity = 128;
/** All bits set mark a catalyst relation with no completion-value requirement. */
inline constexpr std::uint16_t kUnavailableCompletionValueIndex = 0xFFFFU;
/** All bits set mark a catalyst relation with no completion-flag requirement. */
inline constexpr std::uint16_t kUnavailableCompletionFlagIndex = 0xFFFFU;
/** All bits set mark a catalyst relation with no acquired-state gate. */
inline constexpr std::uint16_t kUnavailableAcquisitionIndex = 0xFFFFU;
/** All bits set mark a catalyst relation with no account objective. */
inline constexpr std::uint16_t kUnavailableObjectiveIndex = 0xFFFFU;
/** No shipped catalyst effect needs more than four distinct Family-5 flags or values. */
inline constexpr std::size_t kCompletionFlagCapacity = 4;
inline constexpr std::size_t kCompletionValueCapacity = 4;

/** One Family-5 signed value that must reach a build-derived minimum. */
struct CompletionValue {
    std::uint16_t index{};
    std::int32_t minimum{};

    [[nodiscard]] constexpr bool operator==(const CompletionValue& other) const noexcept = default;
};

/** All positive Family-5 terms read from one catalyst effect's completion expressions. */
struct CompletionRequirements {
    std::array<std::uint16_t, kCompletionFlagCapacity> flags{};
    std::array<CompletionValue, kCompletionValueCapacity> values{};
    std::uint8_t flagCount{};
    std::uint8_t valueCount{};

    [[nodiscard]] constexpr bool
    operator==(const CompletionRequirements& other) const noexcept = default;
};

/** Optional account objective that marks one legacy catalyst's work complete. */
struct ObjectiveCompletion {
    std::uint16_t definitionIndex{kUnavailableObjectiveIndex};
    std::int32_t value{};

    [[nodiscard]] constexpr bool operator==(const ObjectiveCompletion& other) const noexcept =
        default;
};

/** Release status from the build-scoped Season 11 availability data. */
enum class Availability : std::uint8_t {
    released = 0,
    placeholder = 1,
    unsupported = 2,
};

/**
 * @param availability Stored release state to check.
 * @return True when the value is one of the declared states.
 */
[[nodiscard]] constexpr bool valid_availability(Availability availability) noexcept {
    return availability == Availability::released || availability == Availability::placeholder
           || availability == Availability::unsupported;
}

/** Safe catalog and resolver outcomes. */
enum class Error : std::uint8_t {
    none = 0,
    noCatalyst,
    placeholderOnly,
    unsupportedBuild,
    missingReleased,
    ambiguousLifecycle,
    invalidSocket,
    invalidAcquisition,
    invalidCompletion,
    invalidObjective,
    capacityExceeded,
};

/**
 * @param error Catalog or item-resolution result.
 * @return Stable log name for the result.
 */
[[nodiscard]] constexpr std::string_view error_name(Error error) noexcept {
    switch (error) {
    case Error::none:
        return "none";
    case Error::noCatalyst:
        return "no_catalyst";
    case Error::placeholderOnly:
        return "placeholder_only";
    case Error::unsupportedBuild:
        return "unsupported_build";
    case Error::missingReleased:
        return "missing_released";
    case Error::ambiguousLifecycle:
        return "ambiguous_lifecycle";
    case Error::invalidSocket:
        return "invalid_socket";
    case Error::invalidAcquisition:
        return "invalid_acquisition";
    case Error::invalidCompletion:
        return "invalid_completion";
    case Error::invalidObjective:
        return "invalid_objective";
    case Error::capacityExceeded:
        return "capacity_exceeded";
    }
    return "unknown";
}

/** One build-derived exotic weapon catalyst relation. */
struct Definition {
    std::uint32_t itemDefinitionHash{};
    std::uint16_t itemDefinitionIndex{};
    /** Unsupported rows use the unavailable item-index value. */
    std::uint16_t completedPlugDefinitionIndex{};
    /** Legacy in-progress display plug, or the unavailable item-index value. */
    std::uint16_t progressPlugDefinitionIndex{};
    /** Item row that supplies the completed catalyst's native perks and stat changes. */
    std::uint16_t effectDefinitionIndex{};
    /** Family-5 acquired-state slot that makes the catalyst socket visible. */
    std::uint16_t acquisitionDefinitionIndex{kUnavailableAcquisitionIndex};
    /** Family-5 terms required by the catalyst's active effect item. */
    CompletionRequirements completion{};
    /** Account objective referenced by a legacy progress plug, when present. */
    ObjectiveCompletion objective{};
    std::uint8_t socketLane{};
    Availability availability{Availability::unsupported};

    /**
     * @param other Catalyst definition to compare.
     * @return True when every derived field is equal.
     */
    [[nodiscard]] constexpr bool operator==(const Definition& other) const noexcept = default;
};

/**
 * @param left First catalyst definition.
 * @param right Second catalyst definition.
 * @return True when the first native item index is less than the second.
 */
[[nodiscard]] constexpr bool definition_index_less(const Definition& left,
                                                   const Definition& right) noexcept {
    return left.itemDefinitionIndex < right.itemDefinitionIndex;
}

/** The only state a released catalyst exposes to callers. */
struct CompletedCatalyst {
    std::uint8_t socketLane{};
    std::uint16_t completedPlugDefinitionIndex{};
    std::uint16_t progressPlugDefinitionIndex{};
    std::uint16_t effectDefinitionIndex{};
    std::uint16_t acquisitionDefinitionIndex{kUnavailableAcquisitionIndex};
    CompletionRequirements completion{};
    ObjectiveCompletion objective{};
};

/** Result of scanning one effect item's postfix completion expressions. */
enum class CompletionConditionState : std::uint8_t {
    absent = 0,
    present = 1,
    ambiguous = 2,
};

/** One item definition's build-derived completion flag and value. */
struct CompletionCondition {
    std::uint16_t itemDefinitionIndex{};
    CompletionRequirements completion{};
    /** First objective reference carried by the item, when it has exactly one. */
    std::uint16_t objectiveDefinitionIndex{kUnavailableObjectiveIndex};
    CompletionConditionState state{CompletionConditionState::absent};
};

/** Result of reading one socket type's acquired-state rule. */
enum class AcquisitionState : std::uint8_t {
    absent = 0,
    present = 1,
    ambiguous = 2,
};

/** One socket type's build-derived Family-5 acquisition gate. */
struct AcquisitionGate {
    std::uint16_t socketType{};
    std::uint16_t definitionIndex{kUnavailableAcquisitionIndex};
    AcquisitionState state{AcquisitionState::absent};
};

/** One resolver result. Errors never carry a completed state. */
struct Result {
    Error error{Error::noCatalyst};
    Availability availability{Availability::unsupported};
    CompletedCatalyst completed{};
};

/** Whole-catalog counts plus the first unsafe released relation. */
struct Report {
    std::size_t released{};
    std::size_t placeholder{};
    std::size_t unsupported{};
    Error error{Error::none};
    std::uint32_t itemDefinitionHash{};
    std::uint8_t socketLane{};
};

/** Result of the one catalog-aware catalyst item change. */
enum class ApplyResult : std::uint8_t {
    unchanged = 0,
    completed,
    failed,
};

} // namespace sunrise::state::build_data::items::catalysts
