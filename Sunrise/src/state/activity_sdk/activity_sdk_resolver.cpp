#include <algorithm>

#include "../activity/runtime.h"
#include "runtime.h"

namespace sunrise::state::activity_sdk {
namespace {

/** Finds one activity by its unique validated investment index. */
[[nodiscard]] const format::Activity*
find_activity(const Catalog& catalog, std::uint32_t index, std::uint32_t& row) noexcept {
    row = format::kAbsentIndex;
    const auto activities = catalog.activities();
    for (std::size_t candidate = 0; candidate < activities.size(); ++candidate) {
        if (activities[candidate].activityIndex == index) {
            row = static_cast<std::uint32_t>(candidate);
            return &activities[candidate];
        }
    }
    return nullptr;
}

/**
 * Finds the one mod catalog whose activity is named like the destination.
 * @param row Receives the activity row inside that catalog.
 * @return The catalog, or null when no mod or more than one names it.
 */
[[nodiscard]] Snapshot
find_mod_activity(const activity::destination::DestinationSelection& destination,
                  std::uint32_t& row) noexcept {
    row = format::kAbsentIndex;
    const auto catalogs = mod_catalogs();
    if (catalogs == nullptr) {
        return nullptr;
    }
    Snapshot found;
    for (const Snapshot& catalog : *catalogs) {
        const auto activities = catalog->activities();
        for (std::size_t candidate = 0; candidate < activities.size(); ++candidate) {
            if (!destination_matches(*catalog, activities[candidate], destination)) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = catalog;
            row = static_cast<std::uint32_t>(candidate);
        }
    }
    return found;
}

/** Maps a missing snapshot to its published load reason. */
[[nodiscard]] Status missing_catalog_status() noexcept {
    const Status current = status();
    return current == Status::ready ? Status::catalogInvalid : current;
}

} // namespace

/** Binds one validated catalog row to an exact current session and ActivityClient generation. */
Status resolve(Snapshot catalog, const Selection& selection, BoundView& output) noexcept {
    output = {};
    if (catalog == nullptr) {
        return missing_catalog_status();
    }
    if (selection.matchingLinks == 0) {
        return Status::missingClient;
    }
    if (selection.matchingLinks != 1) {
        return Status::ambiguousClient;
    }
    if (selection.activityClientGeneration == 0) {
        return Status::staleActivityClient;
    }
    if (!activity::binding_matches(selection.binding)) {
        return Status::staleSession;
    }
    if (selection.binding.destination.activityIndex < 0) {
        return Status::wrongActivity;
    }

    std::uint32_t activityRow = format::kAbsentIndex;
    const format::Activity* activityRowValue =
        find_activity(*catalog,
                      static_cast<std::uint32_t>(selection.binding.destination.activityIndex),
                      activityRow);
    if (activityRowValue == nullptr
        || !destination_matches(*catalog, *activityRowValue, selection.binding.destination)) {
        // An activity a mod added is in that mod's own pack.
        catalog = find_mod_activity(selection.binding.destination, activityRow);
        if (catalog == nullptr) {
            return Status::wrongActivity;
        }
        activityRowValue = &catalog->activities()[activityRow];
    }
    if (activityRowValue->scenarioIndex >= catalog->scenarios().size()) {
        return Status::missingScenarioLink;
    }
    if ((activityRowValue->flags & format::kActivityExactMask) != format::kActivityExactMask) {
        return Status::activityJoinNotExact;
    }

    output.catalog = std::move(catalog);
    output.binding = selection.binding;
    output.activityClientGeneration = selection.activityClientGeneration;
    output.activityRow = activityRow;
    output.scenarioRow = activityRowValue->scenarioIndex;
    return Status::ready;
}

/** Refuses a previously bound view as soon as any captured live identity changes. */
Status revalidate(const BoundView& view,
                  const activity::SessionBinding& currentBinding,
                  std::size_t matchingLinks,
                  std::uint64_t currentActivityClientGeneration) noexcept {
    const format::Activity* const activityRow = bound_activity(view);
    if (activityRow == nullptr || bound_scenario(view) == nullptr) {
        return Status::catalogInvalid;
    }
    if (matchingLinks == 0) {
        return Status::missingClient;
    }
    if (matchingLinks != 1) {
        return Status::ambiguousClient;
    }
    if (!activity::binding_matches(currentBinding) || !same_binding(view.binding, currentBinding)) {
        return Status::staleSession;
    }
    if (currentActivityClientGeneration == 0
        || currentActivityClientGeneration != view.activityClientGeneration) {
        return Status::staleActivityClient;
    }
    if (!destination_matches(*view.catalog, *activityRow, currentBinding.destination)) {
        return Status::wrongActivity;
    }
    if (activityRow->scenarioIndex >= view.catalog->scenarios().size()
        || activityRow->scenarioIndex != view.scenarioRow) {
        return Status::missingScenarioLink;
    }
    if ((activityRow->flags & format::kActivityExactMask) != format::kActivityExactMask) {
        return Status::activityJoinNotExact;
    }
    return Status::ready;
}

} // namespace sunrise::state::activity_sdk
