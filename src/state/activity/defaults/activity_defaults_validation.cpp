#include "activity_defaults_validation.h"

#include <limits>
#include <string_view>

#include "../destination/activity_destination_validation.h"

namespace sunrise::state::activity::defaults {
namespace {

/**
 * Builds the low-bit mask for a bounded bubble count, without shifting by 64.
 * @param bubbleCount Count in the inclusive range 1 to 64.
 * @return Mask whose low bubbleCount bits are set.
 */
[[nodiscard]] std::uint64_t bubble_mask(std::uint8_t bubbleCount) noexcept {
    if (bubbleCount == kBubbleCapacity) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return (std::uint64_t{1} << bubbleCount) - 1;
}

/** Package loaded by Sunrise's bundled local fallback when no destination was selected. */
constexpr std::string_view kBundledPackageName = "city_tower_social_d2";
/** -1 records that the fallback did not come from a selection reason. */
constexpr std::int8_t kBundledReason = -1;
/** -1 records that the fallback has no previous activity. */
constexpr std::int16_t kBundledPreviousActivityIndex = -1;
/** Definition index 20 is paired with the bundled fallback package. */
constexpr std::int16_t kBundledActivityIndex = 20;
/** The bundled fallback package declares 8 bubble entries. */
constexpr std::uint8_t kBundledBubbleCount = 8;
/** Bits 0 to 3, 6 and 7 mark the fallback's stateful bubbles. */
constexpr std::uint64_t kBundledStatefulBubbleMask = 0xCF;
/** Slice-state 48 belongs to bubble 6, the local fallback arrival. */
constexpr std::uint16_t kBundledInitialSliceSet = 48;
/** The FNV-1 basis is the local unset hash when no named spawn set is selected. */
constexpr std::uint32_t kBundledSpawnSetHash = 0x811C9DC5;

static_assert(kBundledPackageName.size() <= destination::kPackageNameCapacity);

} // namespace

/** Checks one coherent default destination and its small numeric fallback policy. */
bool valid(const DefaultDestination& candidate) noexcept {
    const FallbackPolicy& fallback = candidate.fallback;
    if (!destination::valid(candidate.selection) || fallback.bubbleCount < kMinimumBubbleCount
        || fallback.bubbleCount > kBubbleCapacity
        || fallback.initialSliceSet > kMaximumInitialSliceSet
        || (fallback.statefulBubbleMask & ~bubble_mask(fallback.bubbleCount)) != 0) {
        return false;
    }

    // 8 consecutive slice-state values belong to one bubble in stable index order.
    const std::size_t initialBubble = fallback.initialSliceSet / kSliceStatesPerBubble;
    return initialBubble < fallback.bubbleCount
           && (fallback.statefulBubbleMask & (std::uint64_t{1} << initialBubble)) != 0;
}

/** Checks all immutable activity defaults supplied to State startup. */
bool valid(const ActivityDefaults& candidate) noexcept {
    return valid(candidate.defaultDestination);
}

/** Builds Sunrise's small local absent-selection policy. */
ActivityDefaults authored() noexcept {
    ActivityDefaults result{};
    destination::DestinationSelection& selection = result.defaultDestination.selection;
    for (std::size_t index = 0; index < kBundledPackageName.size(); ++index) {
        selection.packageName[index] = static_cast<std::int8_t>(kBundledPackageName[index]);
    }
    selection.packageNameLength = static_cast<std::uint8_t>(kBundledPackageName.size());
    selection.reason = kBundledReason;
    selection.previousActivityIndex = kBundledPreviousActivityIndex;
    selection.activityIndex = kBundledActivityIndex;
    // Value initialization intentionally leaves the element and optional hashes absent.

    FallbackPolicy& fallback = result.defaultDestination.fallback;
    fallback.bubbleCount = kBundledBubbleCount;
    fallback.statefulBubbleMask = kBundledStatefulBubbleMask;
    fallback.initialSliceSet = kBundledInitialSliceSet;
    fallback.spawnSetHash = kBundledSpawnSetHash;
    return result;
}

} // namespace sunrise::state::activity::defaults
