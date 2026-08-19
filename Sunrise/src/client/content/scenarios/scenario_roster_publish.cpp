#include <algorithm>
#include <cstddef>

#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/**
 * Orders the safe groups the way the destination publishes them.
 * A group that binds the player or reports the lifetime comes first, then one reached through the
 * destination's own registry array, then the lower key.
 * @return True when left publishes before right.
 */
[[nodiscard]] bool publishes_first(const Candidate& left, const Candidate& right) noexcept {
    const bool leftFilled = left.bindsPlayer || left.reportsLifetime;
    const bool rightFilled = right.bindsPlayer || right.reportsLifetime;
    if (leftFilled != rightFilled) {
        return leftFilled;
    }
    if (left.primaryRegistry != right.primaryRegistry) {
        return left.primaryRegistry;
    }
    return left.key < right.key;
}

/**
 * Writes the top-level half: the candidates whose key is in every slice set.
 * @param walk Accumulator for one destination.
 * @param row Destination row receiving its group indices.
 */
void publish_top_level(Walk& walk, layouts::Definition& row) noexcept {
    std::array<std::uint32_t, tables::kRosterKeyCapacity> safe{};
    std::size_t safeCount = 0;
    if (!tables::safe_roster_keys(walk.intersection, safe, safeCount) || safeCount == 0) {
        return;
    }
    std::array<Candidate, tables::kRosterKeyCapacity> kept{};
    std::size_t keptCount = 0;
    for (std::size_t index = 0; index < walk.candidateCount; ++index) {
        const Candidate& candidate = walk.candidates[index];
        const auto last = safe.begin() + static_cast<std::ptrdiff_t>(safeCount);
        const bool keep = std::find(safe.begin(), last, candidate.key) != last;
        if (keep && keptCount < kept.size()) {
            kept[keptCount++] = candidate;
        }
    }
    std::sort(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(keptCount), publishes_first);
    // A roster missing either filled type seeds nothing the spawn gate reads, so publish none.
    bool binds = false;
    bool reports = false;
    for (std::size_t index = 0; index < keptCount; ++index) {
        binds = binds || kept[index].bindsPlayer;
        reports = reports || kept[index].reportsLifetime;
    }
    if (!binds || !reports) {
        return;
    }
    const std::size_t published = (std::min)(keptCount, layouts::kDestinationGroupCapacity);
    for (std::size_t index = 0; index < published; ++index) {
        row.rosterGroups[index] = kept[index].group;
    }
    row.rosterGroupCount = static_cast<std::uint8_t>(published);
}

/**
 * Writes the per-bubble half: the candidates whose key is in some slice sets and not all.
 * @param walk Accumulator for one destination.
 * @param row Destination row receiving its per-bubble group indices and their bubbles.
 */
void publish_per_bubble(Walk& walk, layouts::Definition& row) noexcept {
    std::array<std::uint32_t, tables::kRosterKeyCapacity> keys{};
    std::array<std::uint64_t, tables::kRosterKeyCapacity> masks{};
    std::size_t partialCount = 0;
    if (!tables::partial_roster_keys(walk.intersection, keys, masks, partialCount)
        || partialCount == 0) {
        return;
    }
    std::size_t published = 0;
    for (std::size_t index = 0;
         index < walk.candidateCount && published < layouts::kDestinationBubbleGroupCapacity;
         ++index) {
        const Candidate& candidate = walk.candidates[index];
        for (std::size_t partial = 0; partial < partialCount; ++partial) {
            if (keys[partial] != candidate.key) {
                continue;
            }
            row.bubbleGroups[published] = candidate.group;
            row.bubbleGroupMasks[published] = masks[partial];
            ++published;
            break;
        }
    }
    row.bubbleGroupCount = static_cast<std::uint8_t>(published);
}

} // namespace

/** Splits the candidates between the destination row's two lists. */
void publish_groups(Walk& walk, layouts::Definition& row) noexcept {
    row.rosterGroupCount = 0;
    row.rosterGroups = {};
    row.bubbleGroupCount = 0;
    row.bubbleGroups = {};
    row.bubbleGroupMasks = {};
    publish_top_level(walk, row);
    // The per-bubble half is independent of the top-level one: its keys register through the
    // delta's own field 1, and a destination may reach one half and not the other.
    publish_per_bubble(walk, row);
}

} // namespace sunrise::client::content::scenarios
