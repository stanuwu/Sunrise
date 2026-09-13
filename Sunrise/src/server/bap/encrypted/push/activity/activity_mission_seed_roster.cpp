#include "activity_mission_seed_roster.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/activity_sdk/generated_world/runtime.h"
#include "../../../../../state/activity_sdk/runtime.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace layouts = state::build_data::scenarios;

/** Logs which exit refused, since the returned outcome itself carries no reason. */
[[nodiscard]] MissionSeedRosterResult refuse_seed(std::string_view reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=mission_seed_refusal reason=%.*s",
                                      static_cast<int>(reason.size()),
                                      reason.data());
    if (written > 0) {
        // A refused seed leaves the selected state unpublished, so the refusal is not debug volume.
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return MissionSeedRosterResult::refused;
}
namespace sdk = state::activity_sdk;

/** @return True when two complete groups have byte-identical wire fields. */
[[nodiscard]] bool same_group(const layouts::RosterGroup& left,
                              const layouts::RosterGroup& right) noexcept {
    if (!layouts::valid_roster_group(left) || !layouts::valid_roster_group(right)
        || left.objectTag != right.objectTag || left.registryKey != right.registryKey
        || left.slotCount != right.slotCount) {
        return false;
    }
    for (std::size_t slot = 0; slot < left.slotCount; ++slot) {
        if (left.slotTypes[slot] != right.slotTypes[slot]
            || left.slotFlags[slot] != right.slotFlags[slot]
            || left.slotIndices[slot] != right.slotIndices[slot]) {
            return false;
        }
    }
    return true;
}

/** @return True when the retained lease still matches the materializer's exact scalar summary. */
[[nodiscard]] bool same_plan(const ActivityMissionSeedPlan& plan,
                             const sdk::MissionSeedSummary& summary) noexcept {
    if (plan.omissionCount != summary.omissionCount) {
        return false;
    }
    for (std::uint32_t index = 0; index < plan.omissionCount; ++index) {
        if (plan.omissions[index].objectTag != summary.omissions[index].objectTag
            || plan.omissions[index].registryKey != summary.omissions[index].registryKey) {
            return false;
        }
    }
    return plan.activityRow == summary.activityRow && plan.scenarioRow == summary.scenarioRow
           && plan.stateRow == summary.stateRow && plan.bubbleRow == summary.bubbleRow
           && plan.bubbleOrdinal == summary.bubbleOrdinal
           && plan.stateOrdinal == summary.stateOrdinal && plan.entryIndex == summary.entryIndex
           && plan.sliceSetIndex == summary.sliceSetIndex
           && plan.effectiveRegion == summary.effectiveRegion
           && plan.occurrenceCount == summary.occurrenceCount
           && plan.groupCount == summary.groupCount
           && plan.authMappingSlots == summary.authMappingSlots
           && plan.authResetSlots == summary.authResetSlots
           && plan.senseSuppressedSlots == summary.senseSuppressedSlots;
}

/** Copies one generated selected-state summary into its connection-local transport lease. */
[[nodiscard]] ActivityMissionSeedPlan plan_from(const sdk::MissionSeedSummary& summary) noexcept {
    ActivityMissionSeedPlan plan{};
    plan.activityRow = summary.activityRow;
    plan.scenarioRow = summary.scenarioRow;
    plan.stateRow = summary.stateRow;
    plan.bubbleRow = summary.bubbleRow;
    plan.bubbleOrdinal = summary.bubbleOrdinal;
    plan.stateOrdinal = summary.stateOrdinal;
    plan.entryIndex = summary.entryIndex;
    plan.sliceSetIndex = summary.sliceSetIndex;
    plan.effectiveRegion = summary.effectiveRegion;
    plan.omissions = summary.omissions;
    plan.omissionCount = summary.omissionCount;
    plan.occurrenceCount = summary.occurrenceCount;
    plan.groupCount = summary.groupCount;
    plan.authMappingSlots = summary.authMappingSlots;
    plan.authResetSlots = summary.authResetSlots;
    plan.senseSuppressedSlots = summary.senseSuppressedSlots;
    return plan;
}

/** Copies one materialized group into the encoder input. */
[[nodiscard]] bool append_group(const layouts::RosterGroup& source,
                                Scratch& scratch,
                                message::Roster& roster) noexcept {
    if (!layouts::valid_roster_group(source) || roster.groupCount >= roster.groups.size()
        || roster.groupCount >= scratch.rosterGroups.size()) {
        return false;
    }
    const std::size_t position = roster.groupCount;
    layouts::RosterGroup& stored = scratch.rosterGroups[position];
    stored = source;
    message::Group& group = roster.groups[position];
    group.objectTag = stored.objectTag;
    group.key = stored.registryKey;
    group.slotTypes = std::span<const std::uint8_t>(stored.slotTypes.data(), stored.slotCount);
    group.slotFlags = std::span<const std::uint8_t>(stored.slotFlags.data(), stored.slotCount);
    group.slotIndices = std::span<const std::uint16_t>(stored.slotIndices.data(), stored.slotCount);
    ++roster.groupCount;
    return true;
}

/** Adds one key to an exact bubble sub-block, deduping an already-active key. */
[[nodiscard]] bool append_bubble_key(std::uint32_t bubble,
                                     std::uint32_t key,
                                     Scratch& scratch,
                                     message::Roster& roster) noexcept {
    std::size_t block = 0;
    while (block < roster.bubbleSubBlocks.size()
           && roster.bubbleSubBlocks[block].bubble != bubble) {
        ++block;
    }
    if (block == roster.bubbleSubBlocks.size()) {
        if (block >= scratch.rosterSubBlocks.size()) {
            return false;
        }
        scratch.rosterSubBlocks[block].bubble = bubble;
        scratch.rosterSubBlockKeys[block][0] = key;
        scratch.rosterSubBlocks[block].keys =
            std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[block].data(), 1);
        roster.bubbleSubBlocks = std::span(scratch.rosterSubBlocks).first(block + 1);
        return true;
    }
    for (const std::uint32_t existing : roster.bubbleSubBlocks[block].keys) {
        if (existing == key) {
            return true;
        }
    }
    const std::size_t keyCount = scratch.rosterSubBlocks[block].keys.size();
    if (keyCount >= scratch.rosterSubBlockKeys[block].size()) {
        return false;
    }
    scratch.rosterSubBlockKeys[block][keyCount] = key;
    scratch.rosterSubBlocks[block].keys =
        std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[block].data(), keyCount + 1);
    return true;
}

/** @return True when two SDK scene rows name the same outbound object slot. */
[[nodiscard]] bool same_scene_target(const sdk::AuthoredSceneSeed& left,
                                     const sdk::AuthoredSceneSeed& right) noexcept {
    return left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.slotType == right.slotType && left.slotIndex == right.slotIndex;
}

/** @return True when two SDK scene rows are the same complete seed. */
[[nodiscard]] bool same_scene_seed(const sdk::AuthoredSceneSeed& left,
                                   const sdk::AuthoredSceneSeed& right) noexcept {
    return same_scene_target(left, right) && left.authSchema == right.authSchema
           && left.resourceTag == right.resourceTag;
}

/** Collects and deduplicates exact type-43 seeds from the selected state occurrences. */
[[nodiscard]] bool collect_scene_seeds(const sdk::BoundView& view,
                                       const sdk::MissionSeedSummary& summary,
                                       Scratch& scratch,
                                       std::size_t& outputCount) noexcept {
    outputCount = 0;
    if (view.catalog == nullptr) {
        return false;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const sdk::format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto objects = catalog.objects();
    if (scenario == nullptr) {
        return false;
    }

    for (const sdk::format::Occurrence& occurrence :
         sdk::scenario_occurrences(catalog, *scenario)) {
        if (occurrence.stateIndex != summary.stateRow) {
            continue;
        }
        if (occurrence.scenarioIndex != summary.scenarioRow
            || occurrence.bubbleIndex != summary.bubbleRow
            || occurrence.objectIndex >= objects.size()) {
            return false;
        }
        const sdk::format::Object& object = objects[occurrence.objectIndex];
        bool hasSceneSlot = false;
        for (const sdk::format::Slot& slot : sdk::object_slots(catalog, object)) {
            hasSceneSlot = hasSceneSlot
                           || (slot.slotType == sdk::format::kAuthoredSceneSlotType
                               && slot.componentClass != sdk::format::kAbsentIndex);
        }
        if (!hasSceneSlot) {
            continue;
        }
        if (outputCount >= scratch.rosterSceneSeeds.size()) {
            return false;
        }

        const std::size_t first = outputCount;
        std::size_t produced = 0;
        const auto available = std::span(scratch.rosterSceneSeeds).subspan(first);
        if (sdk::materialize_authored_scene_seeds(catalog, object, available, produced)
                != sdk::AuthoredSceneSeedStatus::ready
            || produced > available.size()) {
            return false;
        }

        std::size_t uniqueCount = first;
        for (std::size_t index = 0; index < produced; ++index) {
            const sdk::AuthoredSceneSeed candidate = scratch.rosterSceneSeeds[first + index];
            bool duplicate = false;
            for (std::size_t earlier = 0; earlier < uniqueCount; ++earlier) {
                const sdk::AuthoredSceneSeed& retained = scratch.rosterSceneSeeds[earlier];
                if (!same_scene_target(retained, candidate)) {
                    continue;
                }
                if (!same_scene_seed(retained, candidate)) {
                    return false;
                }
                duplicate = true;
                break;
            }
            if (!duplicate) {
                scratch.rosterSceneSeeds[uniqueCount++] = candidate;
            }
        }
        outputCount = uniqueCount;
    }
    return true;
}

/** Counts the Auth slots among these groups that a scene seed names. */
[[nodiscard]] std::size_t seed_slot_matches(const sdk::AuthoredSceneSeed& seed,
                                            std::span<const layouts::RosterGroup> groups) noexcept {
    std::size_t matches = 0;
    for (const layouts::RosterGroup& group : groups) {
        if (group.objectTag != seed.objectTag || group.registryKey != seed.registryKey) {
            continue;
        }
        for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
            matches += group.slotTypes[slot] == seed.slotType
                               && group.slotIndices[slot] == seed.slotIndex
                               && (group.slotFlags[slot] & layouts::kSlotAuthFlag) != 0
                           ? 1U
                           : 0U;
        }
    }
    return matches;
}

/**
 * Checks every scene seed's shape and refuses one that names more than one materialized slot.
 * A seed whose object is not materialized waits; the game builds that object itself.
 */
[[nodiscard]] bool validate_scene_targets(std::span<const sdk::AuthoredSceneSeed> seeds,
                                          std::span<const layouts::RosterGroup> groups) noexcept {
    for (const sdk::AuthoredSceneSeed& seed : seeds) {
        if (seed.objectTag == 0 || seed.registryKey == 0 || seed.resourceTag == 0
            || seed.resourceTag == sdk::format::kAbsentIndex
            || seed.slotType != message::kAuthoredSceneSlotType
            || seed.slotIndex > message::kMaximumSlotIndex
            || seed.authSchema != message::kAuthoredSceneAuthSchema
            || seed_slot_matches(seed, groups) > 1) {
            return false;
        }
    }
    return true;
}

/**
 * Installs the baseline authored-scene bodies whose roster groups this body publishes. A seed
 * whose group is not published yet waits for that group; it does not refuse the roster.
 */
[[nodiscard]] bool install_scene_seeds(std::span<const sdk::AuthoredSceneSeed> seeds,
                                       Scratch& scratch,
                                       message::Snapshot& snapshot) noexcept {
    const auto published = std::span<const layouts::RosterGroup>(scratch.rosterGroups)
                               .first(snapshot.roster.groupCount);
    if (snapshot.authOverrides.size() > scratch.rosterAuthOverrides.size()) {
        return false;
    }
    for (const sdk::AuthoredSceneSeed& seed : seeds) {
        const std::size_t matches = seed_slot_matches(seed, published);
        if (matches > 1) {
            return false;
        }
        if (matches == 0) {
            continue;
        }
        for (const message::AuthOverride& retained : snapshot.authOverrides) {
            if (retained.objectTag == seed.objectTag && retained.key == seed.registryKey
                && retained.slotType == seed.slotType && retained.slotIndex == seed.slotIndex) {
                return false;
            }
        }
        const std::size_t position = snapshot.authOverrides.size();
        if (position >= scratch.rosterAuthOverrides.size()) {
            return false;
        }
        message::AuthOverride value{};
        // The descriptor owns the scene resource. Auth begins with a signed, bias-0x80000000
        // activation generation, so wire 0x80000000 is decoded generation zero: baseline only.
        value.body[0] = std::byte{0x80};
        value.objectTag = seed.objectTag;
        value.key = seed.registryKey;
        value.authSchema = seed.authSchema;
        value.slotIndex = static_cast<std::uint16_t>(seed.slotIndex);
        value.bitCount = message::kAuthoredSceneAuthBitCount;
        value.slotType = static_cast<std::uint8_t>(seed.slotType);
        value.byteCount = message::kAuthoredSceneAuthByteCount;
        value.present = true;
        scratch.rosterAuthOverrides[position] = value;
        snapshot.authOverrides = std::span(scratch.rosterAuthOverrides).first(position + 1);
    }
    return true;
}

} // namespace

/** Adds the destination's generated selected-state roster to the initial publication. */
MissionSeedRosterResult append_initial_mission_seed(Session& session,
                                                    Scratch& scratch,
                                                    message::Snapshot& snapshot,
                                                    std::uint32_t effectiveRegion,
                                                    std::uint64_t hostedBubbles,
                                                    std::size_t canonicalGroupCount,
                                                    const RefreshReport* refresh) noexcept {
    MissionSeedLease& lease = session.activityMissionSeed;
    if (lease.configured && lease.bindingGeneration != session.activity.bindingGeneration) {
        lease = {};
    }

    const bool adopting = !lease.configured;
    if (adopting && session.activityRosterSends != 0) {
        return MissionSeedRosterResult::inactive;
    }
    if (canonicalGroupCount > scratch.rosterGroups.size()
        || canonicalGroupCount != snapshot.roster.groupCount) {
        return refuse_seed("canonical_count");
    }

    const sdk::Snapshot catalog = sdk::snapshot();
    sdk::BoundView view{};
    // This builder already owns one exact lock-held ActivityClient. Global binding multiplicity is
    // a panel-action concern; it must not stop each peer connection from publishing its roster.
    const sdk::Selection selection{session.activity.session, 1, session.activity.bindingGeneration};
    if (catalog == nullptr || sdk::resolve(catalog, selection, view) != sdk::Status::ready) {
        return refuse_seed("sdk_resolve");
    }

    // A region-changing selection publishes nothing of its new region until the client's current
    // leg names it, or the registration races the old world's teardown. The roster epoch is not
    // advanced here; it follows the bubble the client holds.
    const state::activity::membership::ClientPlacement placement =
        client_placement(session, refresh);
    const std::int32_t heldRegion = state::activity::membership::instantiated_region(placement);
    // The window closes on the exact packed region, so a sibling state of one bubble counts.
    if (!adopting && lease.regionArrivalPending
        && mission_seed_arrival_window_closed(heldRegion, lease.plan.effectiveRegion)) {
        lease.regionArrivalPending = false;
    }
    const bool arrivalWindow = !adopting && lease.regionArrivalPending;
    const ActivityMissionSeedPlan& activePlan = arrivalWindow ? lease.previousPlan : lease.plan;
    const std::uint32_t selectedRegion =
        lease.configured ? activePlan.effectiveRegion : effectiveRegion;
    if (selectedRegion > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return refuse_seed("region_range");
    }
    // The selected state's groups are its bubble's content, so a link that does not host that
    // bubble publishes none of them.
    const std::uint32_t selectedBubble =
        selectedRegion / middleware::content::packages::tables::kSliceSetIndexFactor;
    if (selectedBubble >= layouts::kBubbleCapacity
        || ((hostedBubbles >> selectedBubble) & 1U) == 0) {
        return MissionSeedRosterResult::inactive;
    }
    const std::size_t available = scratch.rosterGroups.size() - canonicalGroupCount;
    const auto materialized =
        std::span(scratch.rosterGroups).subspan(canonicalGroupCount, available);
    // Every re-materialization uses the lease's own omissions, or the published plan stops
    // matching the lease and the whole seed is refused.
    const std::span<const sdk::MissionSeedOmission> omissions =
        std::span(activePlan.omissions).first(lease.configured ? activePlan.omissionCount : 0);
    sdk::MissionSeedSummary summary{};
    if (sdk::materialize_initial_mission_seed(
            view, static_cast<std::int32_t>(selectedRegion), omissions, materialized, summary)
            != sdk::MissionSeedStatus::ready
        || summary.groupCount > materialized.size()
        || summary.bubbleOrdinal >= layouts::kBubbleCapacity) {
        return refuse_seed("seed_not_ready");
    }
    const ActivityMissionSeedPlan plan = adopting ? plan_from(summary) : activePlan;
    if ((!adopting
         && (view.scenarioRow != plan.scenarioRow || view.activityRow != plan.activityRow
             || !same_plan(plan, summary)))
        || plan.effectiveRegion != selectedRegion) {
        return refuse_seed("plan_mismatch");
    }
    const std::uint64_t revision = adopting ? 1 : lease.revision;
    const std::uint64_t publishedRevision = adopting ? 0 : lease.publishedRevision;
    if (!mission_seed_publication_region_ready(
            true, revision, publishedRevision, plan.effectiveRegion, selectedRegion)) {
        return refuse_seed("publication_not_ready");
    }

    std::size_t sceneSeedCount = 0;
    if (!collect_scene_seeds(view, summary, scratch, sceneSeedCount)
        || sceneSeedCount > scratch.rosterSceneSeeds.size()
        || !validate_scene_targets(std::span(scratch.rosterSceneSeeds).first(sceneSeedCount),
                                   materialized.first(summary.groupCount))) {
        return refuse_seed("scene_seeds");
    }

    // A publication must not shrink the registered group set: a group left out is never torn down
    // and its records stay unseeded. So every publication carries the union of the registered
    // regions' groups, activated through one sub-block per bubble.
    std::array<std::uint8_t, message::kPublishedGroupCapacity> groupBubble{};
    std::array<bool, message::kPublishedGroupCapacity> groupActive{};
    std::size_t foldGroupCount = summary.groupCount;
    for (std::size_t index = 0; index < foldGroupCount; ++index) {
        groupBubble[index] = static_cast<std::uint8_t>(summary.bubbleOrdinal);
        groupActive[index] = true;
    }
    for (std::size_t index = 0; index < lease.registeredRegionCount; ++index) {
        const std::uint32_t region = lease.registeredRegions[index];
        if (region == selectedRegion) {
            continue;
        }
        // The pending region's groups wait for arrival with the rest of its content.
        if (arrivalWindow && region == lease.plan.effectiveRegion) {
            continue;
        }
        const auto tail = materialized.subspan(foldGroupCount);
        sdk::MissionSeedSummary retained{};
        if (sdk::materialize_initial_mission_seed(
                view, static_cast<std::int32_t>(region), omissions, tail, retained)
                != sdk::MissionSeedStatus::ready
            || retained.groupCount > tail.size() || retained.scenarioRow != summary.scenarioRow
            || retained.bubbleOrdinal >= layouts::kBubbleCapacity) {
            return refuse_seed("retained_region");
        }
        // A link publishes only the bubbles it hosts; another link carries the rest.
        if (((hostedBubbles >> retained.bubbleOrdinal) & 1U) == 0) {
            continue;
        }
        for (std::size_t group = 0; group < retained.groupCount; ++group) {
            groupBubble[foldGroupCount + group] = static_cast<std::uint8_t>(retained.bubbleOrdinal);
            // Definitions remain registered, but an older state in this bubble is no longer active.
            groupActive[foldGroupCount + group] = retained.bubbleOrdinal != summary.bubbleOrdinal;
        }
        foldGroupCount += retained.groupCount;
    }

    std::array<std::uint16_t, message::kPublishedGroupCapacity> appendRows{};
    std::array<std::uint32_t, message::kBubbleKeyCapacity> managedKeys{};
    std::array<std::uint8_t, message::kBubbleKeyCapacity> managedBubbles{};
    std::array<bool, message::kBubbleKeyCapacity> managedActive{};
    std::array<std::uint32_t, message::kBubbleKeyCapacity> activationKeys{};
    std::array<std::uint8_t, message::kBubbleKeyCapacity> activationBubbles{};
    std::size_t appendCount = 0;
    std::size_t managedCount = 0;
    std::size_t activationCount = 0;
    // Only a mission script selects a state, so an adopted default plan publishes the
    // scenario-wide groups alone. A public region's generated groups go out with a selected plan's
    // once the client reports holding it, and the published set never shrinks afterward.
    bool publicRegion = false;
    {
        sdk::generated_world::GeneratedWorldView worldView{};
        bool isPublic = false;
        if (sdk::generated_world::resolve(view, worldView)
                == sdk::generated_world::BindStatus::ready
            && sdk::generated_world::region_is_public(
                worldView, static_cast<std::int32_t>(selectedRegion), isPublic)) {
            publicRegion = isPublic;
        }
    }
    const bool transitionPublication = mission_seed_transition_subset_only(
        lease.fullSetPublished, lease.scriptSelected, publicRegion, heldRegion, selectedRegion);
    for (std::size_t source = 0; source < foldGroupCount; ++source) {
        const layouts::RosterGroup& candidate = materialized[source];
        if (!layouts::valid_roster_group(candidate)) {
            return refuse_seed("invalid_group");
        }
        // A group the catalog cannot place in every enabled state waits with the state-local
        // ones. Refusing the whole roster here strands the load on task 9.
        bool scenarioWide = false;
        if (transitionPublication
            && !sdk::mission_seed_group_is_scenario_wide(
                view, candidate.objectTag, candidate.registryKey, scenarioWide)) {
            scenarioWide = false;
        }
        if (transitionPublication && !scenarioWide) {
            continue;
        }
        bool priorCandidate = false;
        for (std::size_t prior = 0; prior < source; ++prior) {
            const layouts::RosterGroup& earlier = materialized[prior];
            if (earlier.registryKey != candidate.registryKey) {
                continue;
            }
            if (!same_group(earlier, candidate)) {
                return refuse_seed("duplicate_group");
            }
            priorCandidate = true;
            break;
        }
        if (priorCandidate) {
            continue;
        }

        std::size_t canonicalPosition = canonicalGroupCount;
        for (std::size_t existing = 0; existing < canonicalGroupCount; ++existing) {
            const layouts::RosterGroup& canonical = scratch.rosterGroups[existing];
            if (canonical.registryKey != candidate.registryKey) {
                continue;
            }
            if (!same_group(canonical, candidate)) {
                return refuse_seed("canonical_group_mismatch");
            }
            if (canonicalPosition != canonicalGroupCount) {
                return refuse_seed("canonical_position");
            }
            canonicalPosition = existing;
        }
        const bool canonicalTopLevel = canonicalPosition < snapshot.roster.topLevelGroupCount;
        if (!canonicalTopLevel) {
            if (managedCount >= managedKeys.size()) {
                return refuse_seed("managed_key_capacity");
            }
            managedBubbles[managedCount] = groupBubble[source];
            managedKeys[managedCount] = candidate.registryKey;
            managedActive[managedCount++] = groupActive[source];
        }
        if (canonicalPosition == canonicalGroupCount) {
            if (appendCount >= appendRows.size()) {
                return refuse_seed("append_capacity");
            }
            appendRows[appendCount++] = static_cast<std::uint16_t>(source);
        }
        if (!canonicalTopLevel && groupActive[source]) {
            if (activationCount >= activationKeys.size()) {
                return refuse_seed("activation_capacity");
            }
            activationBubbles[activationCount] = groupBubble[source];
            activationKeys[activationCount++] = candidate.registryKey;
        }
    }
    if (appendCount > snapshot.roster.groups.size() - snapshot.roster.groupCount) {
        return refuse_seed("append_total");
    }

    // Canonical assembly can inherit the previous state's active key. Replace every mission-owned
    // key in place before adding the selected set; retaining definitions must not retain
    // activation.
    for (std::size_t blockIndex = 0; blockIndex < snapshot.roster.bubbleSubBlocks.size();
         ++blockIndex) {
        const message::BubbleSubBlock& sourceBlock = snapshot.roster.bubbleSubBlocks[blockIndex];
        if (blockIndex >= scratch.rosterSubBlocks.size()) {
            return refuse_seed("managed_block_capacity");
        }
        std::size_t retainedCount = 0;
        for (const std::uint32_t key : sourceBlock.keys) {
            bool managed = false;
            bool active = false;
            for (std::size_t index = 0; index < managedCount; ++index) {
                if (managedBubbles[index] == sourceBlock.bubble && managedKeys[index] == key) {
                    managed = true;
                    active = managedActive[index];
                    break;
                }
            }
            if (managed && !active) {
                // Removal is a cleared presence bit at the old key ordinal, not omission.
                for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
                    if (snapshot.roster.groups[group].key == key) {
                        snapshot.roster.groups[group].retired = true;
                    }
                }
            }
            if (retainedCount >= scratch.rosterSubBlockKeys[blockIndex].size()) {
                return refuse_seed("managed_key_capacity");
            }
            scratch.rosterSubBlockKeys[blockIndex][retainedCount++] = key;
        }
        scratch.rosterSubBlocks[blockIndex].bubble = sourceBlock.bubble;
        scratch.rosterSubBlocks[blockIndex].keys = std::span<const std::uint32_t>(
            scratch.rosterSubBlockKeys[blockIndex].data(), retainedCount);
    }

    // Keep only the selected keys the rebuilt sub-block does not already carry, then check every
    // bubble's key capacity and the room for sub-blocks that do not exist yet.
    std::size_t missingKeyCount = 0;
    for (std::size_t index = 0; index < activationCount; ++index) {
        bool present = false;
        for (const message::BubbleSubBlock& block : snapshot.roster.bubbleSubBlocks) {
            if (block.bubble != activationBubbles[index]) {
                continue;
            }
            for (const std::uint32_t key : block.keys) {
                present = present || key == activationKeys[index];
            }
        }
        if (!present) {
            activationBubbles[missingKeyCount] = activationBubbles[index];
            activationKeys[missingKeyCount++] = activationKeys[index];
        }
    }
    activationCount = missingKeyCount;
    std::size_t newBlocks = 0;
    for (std::size_t index = 0; index < activationCount; ++index) {
        bool firstOfBubble = true;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            firstOfBubble = firstOfBubble && activationBubbles[earlier] != activationBubbles[index];
        }
        if (!firstOfBubble) {
            continue;
        }
        std::size_t existingKeys = 0;
        bool hasBubbleBlock = false;
        for (const message::BubbleSubBlock& block : snapshot.roster.bubbleSubBlocks) {
            if (block.bubble != activationBubbles[index]) {
                continue;
            }
            if (hasBubbleBlock) {
                return refuse_seed("duplicate_bubble_block");
            }
            hasBubbleBlock = true;
            existingKeys = block.keys.size();
        }
        std::size_t missing = 0;
        for (std::size_t other = index; other < activationCount; ++other) {
            missing += activationBubbles[other] == activationBubbles[index] ? 1U : 0U;
        }
        if (existingKeys > message::kBubbleKeyCapacity
            || missing > message::kBubbleKeyCapacity - existingKeys) {
            return refuse_seed("key_capacity");
        }
        newBlocks += hasBubbleBlock ? 0U : 1U;
    }
    if (newBlocks > scratch.rosterSubBlocks.size() - snapshot.roster.bubbleSubBlocks.size()) {
        return refuse_seed("key_capacity");
    }

    for (std::size_t index = 0; index < appendCount; ++index) {
        const layouts::RosterGroup source = materialized[appendRows[index]];
        const std::size_t position = snapshot.roster.groupCount;
        if (!append_group(source, scratch, snapshot.roster)) {
            return refuse_seed("append_group");
        }
        snapshot.roster.groups[position].missionSeedOnly = true;
    }
    for (std::size_t index = 0; index < activationCount; ++index) {
        if (!append_bubble_key(
                activationBubbles[index], activationKeys[index], scratch, snapshot.roster)) {
            return refuse_seed("bubble_key");
        }
    }
    if (!install_scene_seeds(
            std::span(scratch.rosterSceneSeeds).first(sceneSeedCount), scratch, snapshot)) {
        return refuse_seed("scene_install");
    }

    if (adopting) {
        lease = {};
        lease.plan = plan;
        lease.bindingGeneration = session.activity.bindingGeneration;
        lease.revision = revision;
        // The peer registers this state's groups now, so a later selection must keep carrying them.
        lease.registeredRegions[0] = plan.effectiveRegion;
        lease.registeredRegionCount = 1;
        lease.configured = true;
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=activity stage=mission_seed result=adopted region=%u "
                                          "occurrences=%u groups=%u unreplicated=%u incomplete=%u "
                                          "scenes=%zu",
                                          static_cast<unsigned>(plan.effectiveRegion),
                                          static_cast<unsigned>(summary.occurrenceCount),
                                          static_cast<unsigned>(summary.groupCount),
                                          static_cast<unsigned>(summary.unreplicatedObjectCount),
                                          static_cast<unsigned>(summary.incompleteObjectCount),
                                          sceneSeedCount);
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    lease.fullSetPublished = lease.fullSetPublished || !transitionPublication;
    return MissionSeedRosterResult::ready;
}

} // namespace sunrise::server::bap::encrypted::push::activity
