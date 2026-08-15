#include "activity_arrival.h"

#include "../../../../../middleware/content/packages/tables/region_reader.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace tables = middleware::content::packages::tables;
namespace scenarios = state::build_data::scenarios;

/** No bubble of the destination answered, so nothing can be derived from its layout. */
constexpr std::size_t kNoBubble = static_cast<std::size_t>(-1);

/** @return The authored default destination's package name. */
[[nodiscard]] std::string_view
authored_name(const state::activity::defaults::DefaultDestination& defaults) noexcept {
    return {reinterpret_cast<const char*>(defaults.selection.packageName.data()),
            defaults.selection.packageNameLength};
}

/** @param layout Extracted layout. @return Bubbles the layout really declares. */
[[nodiscard]] std::size_t declared_bubbles(const scenarios::Definition& layout) noexcept {
    return (std::min)(static_cast<std::size_t>(layout.bubbleCount), layout.bubbleStates.size());
}

/**
 * Tests whether one bubble declares a slice-set state.
 * A bubble with the enabled state byte has a slice set; one with the disabled byte has none. So
 * this also tests whether its slice set is a usable index.
 * @param layout Extracted layout.
 * @param bubble Bubble ordinal.
 * @return True when the bubble has a slice set.
 */
[[nodiscard]] bool bubble_is_live(const scenarios::Definition& layout,
                                  std::size_t bubble) noexcept {
    return bubble < declared_bubbles(layout)
           && layout.bubbleStates[bubble] == scenarios::kBubbleEnabledByte;
}

/**
 * Finds the bubble whose own name hash the client named.
 * @param layout Extracted layout.
 * @param hash Arrival bubble name hash from the client's request.
 * @return Its bubble ordinal, or no bubble when the layout does not declare that hash.
 */
[[nodiscard]] std::size_t bubble_for_hash(const scenarios::Definition& layout,
                                          std::uint32_t hash) noexcept {
    if (hash == 0) {
        return kNoBubble;
    }
    const std::size_t count = declared_bubbles(layout);
    for (std::size_t bubble = 0; bubble < count; ++bubble) {
        if (layout.bubbleHashes[bubble] == hash) {
            return bubble;
        }
    }
    return kNoBubble;
}

/**
 * Finds the first bubble that declares a slice-set state.
 * @param layout Extracted layout.
 * @return Its bubble ordinal, or no bubble when the layout declares none.
 */
[[nodiscard]] std::size_t first_live_bubble(const scenarios::Definition& layout) noexcept {
    const std::size_t count = declared_bubbles(layout);
    for (std::size_t bubble = 0; bubble < count; ++bubble) {
        if (layout.bubbleStates[bubble] == scenarios::kBubbleEnabledByte) {
            return bubble;
        }
    }
    return kNoBubble;
}

/**
 * Finds the arrival bubble.
 *
 * Order: the authored override, the bubble the client named, the authored default's own bubble,
 * then the destination's first live one. Only free-roam destinations name one on the wire.
 *
 * @param defaults Authored default destination and its numeric launch policy.
 * @param selection Destination the session committed, carrying any wire arrival hash.
 * @param name Destination package name.
 * @param layout Extracted layout for that name.
 * @return The arrival bubble ordinal, or no bubble when nothing matches.
 */
[[nodiscard]] std::size_t
resolve_arrival_bubble(const state::activity::defaults::DefaultDestination& defaults,
                       const state::activity::destination::DestinationSelection& selection,
                       std::string_view name,
                       const scenarios::Definition& layout) noexcept {
    if (selection.hasArrivalBubbleOverride) {
        return selection.arrivalBubbleOverride;
    }
    const std::size_t named = selection.hasArrivalBubbleHash
                                  ? bubble_for_hash(layout, selection.arrivalBubbleHash)
                                  : kNoBubble;
    if (named != kNoBubble) {
        return named;
    }
    // The authored index is measured on a live load of the default destination, so it beats the
    // derived rule there and only there. Anywhere else it would be one place's bubble under
    // another place's name, which still gives a valid slice set and so fails silently.
    if (name == authored_name(defaults)) {
        return defaults.fallback.initialSliceSet / tables::kSliceSetIndexFactor;
    }
    return first_live_bubble(layout);
}

} // namespace

/** Finds the slice-set index a destination arrives in. */
std::uint16_t arrival_slice_set(const state::activity::defaults::DefaultDestination& defaults,
                                const state::activity::destination::DestinationSelection& selection,
                                std::string_view name,
                                const scenarios::Definition& layout) noexcept {
    // A forced slice set is the whole point of forcing one, so nothing derived may replace it.
    if (selection.hasSliceSetOverride) {
        return selection.sliceSetOverride;
    }
    std::size_t bubble = resolve_arrival_bubble(defaults, selection, name, layout);
    // A bubble with no slice-set state has no usable index, so the destination's own first live
    // bubble stands in. Never fall back across destinations: another place's index is valid here
    // and would load the wrong geometry with no error anywhere.
    if (!bubble_is_live(layout, bubble)) {
        bubble = first_live_bubble(layout);
    }
    if (bubble == kNoBubble) {
        return defaults.fallback.initialSliceSet;
    }
    return static_cast<std::uint16_t>(tables::region_index(static_cast<std::uint32_t>(bubble)));
}

} // namespace sunrise::server::bap::encrypted::push::activity
