#include "squad_entity_retirement.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <utility>

#include "../../core/logging/log.h"
#include "../../state/activity_sdk/runtime.h"
#include "../../state/build_data/scriptables/scriptable_catalog.h"
#include "../../state/gameplay/external/entity_object_types.h"
#include "../../state/gameplay/external/entity_position_profiles.h"
#include "../activity/host_runtime.h"
#include "entity_identities.h"
#include "peer/peer_transport.h"

namespace sunrise::server::gameplay::squad_entity_retirement {
namespace {
namespace policy = state::gameplay::squad_entity_retirement;
namespace identities = state::gameplay::entity_identity;
SRWLOCK g_lock{SRWLOCK_INIT};
policy::Store g_store;
policy::Store g_propStore;
struct Target final {
    std::uint64_t session{}, revision{}, generation{};
    /** Link the squad's body was delivered on; only a delivery changes it. */
    std::uint64_t deliveredGeneration{};
    policy::Eligibility eligibility{};
};
std::vector<Target> g_targets;
struct PropTransition final {
    identities::Source source{};
    std::uint64_t transition{};
    std::int32_t fromRegion{}, toRegion{};
    std::uint8_t bubble{};
};
std::vector<PropTransition> g_propTransitions;
/** Eight authored states share one bubble's wire authority selector. */
constexpr std::int32_t kStatesPerBubble = 8;
/** One retained transition per admitted source bounds pending lifetime state. */
constexpr std::size_t kMaximumPropTransitions = identities::kSourceCapacity;

/** Resolves native cells through the installed destination map. */
policy::CellBubbles cell_bubbles(const state::activity::SessionBinding& binding) noexcept {
    policy::CellBubbles cells{};
    cells.fill(-1);
    const auto& destination = binding.destination;
    if (destination.packageNameLength > destination.packageName.size()) {
        return cells;
    }
    const std::string_view name(reinterpret_cast<const char*>(destination.packageName.data()),
                                destination.packageNameLength);
    for (std::size_t index = 0; index < cells.size(); ++index) {
        std::uint8_t bubble{};
        if (state::gameplay::entity_position_profiles::lookup_bubble(
                name, static_cast<std::uint16_t>(index), bubble)) {
            cells[index] = bubble;
        }
    }
    return cells;
}
/** Exactly one admitted current view must own the ActivityClient generation. */
bool snapshot(const state::activity::SessionBinding& binding,
              std::uint64_t generation,
              identities::Source& source,
              std::vector<identities::Identity>& rows) noexcept {
    source = {};
    rows.clear();
    if (!generation || !state::activity::binding_matches(binding)) {
        return false;
    }
    std::array<identities::Source, identities::kSourceCapacity> sources{};
    const auto count =
        entity_identities::sources(binding.sessionId, binding.createdRevision, sources);
    if (count > sources.size()) {
        return false;
    }
    std::size_t matches = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (sources[i].activityClientGeneration == generation) {
            source = sources[i];
            ++matches;
        }
    }
    return matches == 1
           && entity_identities::snapshot_source(source, rows) == identities::Result::unchanged
           && state::gameplay::entity_object_types::enrich_snapshot(rows);
}
/** Reports bounded release evidence without granting unknown identities any authority. */
void report_released(const policy::Mask& mask,
                     std::span<const identities::Identity> rows) noexcept {
    if (!core::log::accepts(core::log::Channel::server, core::log::Level::debug)) {
        return;
    }
    // Bounds one release report; the mask still carries the whole selection.
    constexpr std::size_t kMaximumReportedSlots = 64;
    std::size_t shown = 0;
    for (std::size_t slot = 0; slot < rows.size() && shown < kMaximumReportedSlots; ++slot) {
        if ((std::to_integer<unsigned>(mask[slot / 8]) & (1U << (slot % 8))) == 0) {
            continue;
        }
        ++shown;
        const auto& row = rows[slot];
        const auto& actor = row.actorSource;
        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "squad_entity_retirement stage=released slot=%zu known=%u present=%u conflict=%u "
            "inc=%u alloc=%u type=%u cell=%u parent_known=%u parent=%d rsat=0x%08X "
            "object_type=%d source_known=%u source_present=%u source_key=0x%08X "
            "source_type=%u source_index=%u",
            slot,
            unsigned(row.known),
            unsigned(row.present),
            unsigned(row.conflicted),
            unsigned(row.token.incarnation),
            unsigned(row.allocationSequence),
            unsigned(row.type),
            unsigned(row.cell),
            unsigned(row.anchorKnown),
            row.anchorPresent ? int(row.anchor.slot) : -1,
            row.metadata.rsatTag,
            row.metadata.hasObjectType ? int(row.metadata.objectType) : -1,
            unsigned(actor.known),
            unsigned(actor.present),
            actor.key,
            unsigned(actor.type),
            unsigned(actor.index));
        if (count > 0) {
            core::log::write(
                core::log::Channel::server,
                core::log::Level::debug,
                {line.data(), (std::min)(static_cast<std::size_t>(count), line.size() - 1)});
        }
    }
}

/** Missing parents must remain visible when they block a complete retire tree. */
void report_hierarchy_gaps(std::span<const identities::Identity> rows) noexcept {
    if (!core::log::accepts(core::log::Channel::server, core::log::Level::debug)) {
        return;
    }
    // Bounds one hierarchy-gap report.
    constexpr std::size_t kMaximumReportedGaps = 64;
    std::size_t shown = 0;
    for (std::size_t slot = 0; slot < rows.size() && shown < kMaximumReportedGaps; ++slot) {
        const auto& row = rows[slot];
        if (!row.present) {
            continue;
        }
        const char* reason = nullptr;
        if (!row.known || row.conflicted || !row.anchorKnown || row.token.slot != slot) {
            reason = "identity";
        } else if (row.anchorPresent
                   && (row.anchor.slot >= rows.size() || !rows[row.anchor.slot].present)) {
            reason = "missing_parent";
        } else if (row.anchorPresent && rows[row.anchor.slot].token != row.anchor) {
            reason = "parent_lifetime";
        }
        if (reason == nullptr) {
            continue;
        }
        ++shown;
        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "squad_entity_retirement stage=hierarchy_gap slot=%zu "
                                        "reason=%s parent=%d parent_inc=%u",
                                        slot,
                                        reason,
                                        row.anchorPresent ? int(row.anchor.slot) : -1,
                                        unsigned(row.anchor.incarnation));
        if (count > 0) {
            core::log::write(
                core::log::Channel::server,
                core::log::Level::debug,
                {line.data(), (std::min)(static_cast<std::size_t>(count), line.size() - 1)});
        }
    }
}

/** Logs the complete selected count and a bounded prefix of slot indices. */
void report(const char* stage,
            bool result,
            std::uint8_t bubble,
            const policy::Mask* mask = nullptr) {
    std::array<char, core::log::kLineCapacity> line{};
    std::size_t selected = 0;
    if (mask) {
        for (auto byte : *mask) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                selected += (std::to_integer<unsigned>(byte) >> bit) & 1U;
            }
        }
    }
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "squad_entity_retirement stage=%s bubble=%u accepted=%u selected=%zu slots=",
                      stage,
                      unsigned(bubble),
                      result ? 1U : 0U,
                      selected);
    if (prefix <= 0) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix), shown = 0;
    if (mask) {
        for (std::size_t slot = 0; slot < identities::kSlotCapacity && shown < 32; ++slot) {
            if ((std::to_integer<unsigned>((*mask)[slot / 8]) & (1U << (slot % 8))) == 0) {
                continue;
            }
            const int count = std::snprintf(
                line.data() + length, line.size() - length, "%s%zu", shown ? "," : "", slot);
            if (count <= 0 || static_cast<std::size_t>(count) >= line.size() - length) {
                break;
            }
            length += static_cast<std::size_t>(count);
            ++shown;
        }
    }
    if (shown == 0) {
        line[length++] = '-';
    }
    core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), length});
}
} // namespace
/**
 * Captures outgoing map-prop allocations for an explicit same-bubble state reset.
 * @param view Authenticated activity and source generation.
 * @param world Pinned generated package graph for that activity.
 * @param transition Durable mission intent identity.
 * @param fromRegion Instantiated outgoing authored state.
 * @param toRegion Requested authored state.
 * @return Ready only after every captured tree's purge publication has committed.
 */
TransitionStatus begin_placed_transition(const state::activity_sdk::BoundView& view,
                                         const state::build_data::scriptables::Snapshot& world,
                                         std::uint64_t transition,
                                         std::int32_t fromRegion,
                                         std::int32_t toRegion) noexcept {
    namespace data = state::build_data::scriptables;
    const auto& destination = view.binding.destination;
    if (!transition || !view.activityClientGeneration || world.scenarioNameLength == 0
        || world.scenarioNameLength > world.scenarioName.size()
        || destination.packageNameLength > destination.packageName.size()
        || std::string_view(world.scenarioName.data(), world.scenarioNameLength)
               != std::string_view(reinterpret_cast<const char*>(destination.packageName.data()),
                                   destination.packageNameLength)
        || !state::activity::binding_matches(view.binding)) {
        return TransitionStatus::refused;
    }
    AcquireSRWLockShared(&g_lock);
    for (const auto& prior : g_propTransitions) {
        if (prior.source.activitySessionId == view.binding.sessionId
            && prior.source.activityRevision == view.binding.createdRevision
            && prior.source.activityClientGeneration == view.activityClientGeneration
            && prior.transition == transition) {
            const bool pending = g_propStore.pending(prior.source, prior.bubble);
            const bool same = prior.toRegion == toRegion;
            ReleaseSRWLockShared(&g_lock);
            return !same ? TransitionStatus::refused
                         : (pending ? TransitionStatus::pending : TransitionStatus::ready);
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (fromRegion == toRegion && fromRegion >= 0) {
        return TransitionStatus::ready;
    }
    const auto& diagnostics = world.containerPlacementDiagnostics;
    if (fromRegion < 0 || toRegion < 0
        || fromRegion / kStatesPerBubble >= state::activity::bubble_authority::kFallbackBubble
        || fromRegion / kStatesPerBubble != toRegion / kStatesPerBubble
        || world.status != data::BuildStatus::ready || world.coverage != data::BuildCoverage::full
        || !diagnostics.complete || !diagnostics.contextResolved
        || !diagnostics.identityOwnerInventoryComplete
        || !state::gameplay::entity_object_types::available()) {
        return TransitionStatus::refused;
    }
    const auto bubble = static_cast<std::uint8_t>(fromRegion / kStatesPerBubble);
    if (bubble >= world.bubbles.size() || world.bubbles[bubble].isPublic
        || static_cast<std::uint32_t>(fromRegion % kStatesPerBubble)
               >= world.bubbles[bubble].stateCount
        || static_cast<std::uint32_t>(toRegion % kStatesPerBubble)
               >= world.bubbles[bubble].stateCount) {
        return TransitionStatus::refused;
    }
    identities::Source source{};
    std::vector<identities::Identity> rows;
    if (!snapshot(view.binding, view.activityClientGeneration, source, rows)) {
        return TransitionStatus::refused;
    }
    try {
        std::vector<bool> owned(world.containerPlacementLists.size());
        for (const auto& owner : world.containerPlacementOwners) {
            if (owner.listRow < owned.size()
                && data::container_placement_owner_applies(owner, bubble)) {
                owned[owner.listRow] = true;
            }
        }
        std::vector<std::pair<std::uint32_t, std::uint8_t>> definitions;
        for (const auto& placement : world.containerPlacements) {
            if (placement.listRow < owned.size() && owned[placement.listRow] && placement.complete
                && world.containerPlacementLists[placement.listRow].complete
                && placement.placementIdentifierRead && placement.placementIdentifier != 0
                && placement.placementIdentifier != UINT64_MAX
                && std::find(policy::kRetirablePropTypes.begin(),
                             policy::kRetirablePropTypes.end(),
                             placement.objectType)
                       != policy::kRetirablePropTypes.end()) {
                definitions.emplace_back(placement.classListTag, placement.objectType);
            }
        }
        std::sort(definitions.begin(), definitions.end());
        definitions.erase(std::unique(definitions.begin(), definitions.end()), definitions.end());
        std::vector<policy::Eligibility> eligible;
        policy::Mask mask{};
        for (const auto& row : rows) {
            if (!row.known || !row.present || row.conflicted || row.type != 0
                || !row.metadata.hasRsat) {
                continue;
            }
            state::gameplay::entity_object_types::Row object{};
            if (!state::gameplay::entity_object_types::lookup(row.metadata.rsatTag, object)
                || !std::binary_search(definitions.begin(),
                                       definitions.end(),
                                       std::pair{object.definitionTag, object.objectType})) {
                continue;
            }
            policy::Eligibility target{};
            target.rsatTag = object.rsatTag;
            target.bubble = bubble;
            target.enabled = true;
            target.placedProp = true;
            target.objectType = object.objectType;
            if (std::find(eligible.begin(), eligible.end(), target) == eligible.end()) {
                eligible.push_back(target);
            }
        }
        // The script ends these lifetimes; release reports are not needed to authorize their end.
        for (const auto& row : rows) {
            if (row.present && row.token.slot < identities::kSlotCapacity) {
                mask[row.token.slot / 8] |= std::byte(1U << (row.token.slot % 8));
            }
        }
        if (eligible.empty()) {
            return TransitionStatus::refused;
        }
        const auto cells = cell_bubbles(view.binding);
        AcquireSRWLockExclusive(&g_lock);
        for (const auto& old : g_propTransitions) {
            if (old.source.activitySessionId == source.activitySessionId
                && old.source.activityClientGeneration == source.activityClientGeneration
                && g_propStore.pending(old.source, old.bubble)) {
                ReleaseSRWLockExclusive(&g_lock);
                return TransitionStatus::refused;
            }
        }
        std::erase_if(g_propTransitions, [&](const PropTransition& old) {
            if (old.source.activitySessionId != source.activitySessionId) {
                return false;
            }
            g_propStore.invalidate_source(old.source, old.bubble);
            return true;
        });
        if (g_propTransitions.size() == kMaximumPropTransitions) {
            ReleaseSRWLockExclusive(&g_lock);
            return TransitionStatus::refused;
        }
        // Reserve before capturing so allocation failure cannot orphan an active retirement.
        try {
            g_propTransitions.reserve(g_propTransitions.size() + 1);
        } catch (const std::bad_alloc&) {
            ReleaseSRWLockExclusive(&g_lock);
            return TransitionStatus::refused;
        }
        const bool captured =
            g_propStore.capture(source, bubble, mask, rows, eligible, cells, true);
        if (captured) {
            g_propTransitions.push_back({source, transition, fromRegion, toRegion, bubble});
        }
        ReleaseSRWLockExclusive(&g_lock);
        report("placed_transition", captured, bubble);
        return captured ? TransitionStatus::pending : TransitionStatus::refused;
    } catch (const std::bad_alloc&) {
        return TransitionStatus::refused;
    }
}

/** Reports a retained world change without starting a new capture. */
bool placed_transition_pending(const state::activity::SessionBinding& binding,
                               std::uint64_t generation) noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool pending = std::any_of(
        g_propTransitions.begin(), g_propTransitions.end(), [&](const PropTransition& transition) {
            return transition.source.activitySessionId == binding.sessionId
                   && transition.source.activityRevision == binding.createdRevision
                   && transition.source.activityClientGeneration == generation
                   && g_propStore.pending(transition.source, transition.bubble);
        });
    ReleaseSRWLockShared(&g_lock);
    return pending;
}

/** Withdraws the exact mission's captured prop trees without sending a purge. */
void cancel_placed_transition(const state::activity::SessionBinding& binding,
                              std::uint64_t generation,
                              std::uint64_t transition) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    std::erase_if(g_propTransitions, [&](const PropTransition& row) {
        const bool matches = row.source.activitySessionId == binding.sessionId
                             && row.source.activityRevision == binding.createdRevision
                             && row.source.activityClientGeneration == generation
                             && (transition == 0 || row.transition == transition);
        if (matches) {
            g_propStore.invalidate_source(row.source, row.bubble);
        }
        return matches;
    });
    ReleaseSRWLockExclusive(&g_lock);
}
/** Delivered squad choices replace eligibility for their exact authored target. */
void record_delivered_target(const state::activity::SessionBinding& binding,
                             std::uint64_t generation,
                             const activity::host::PendingScriptableOverride& pending) noexcept {
    if (!generation || pending.target.slotType != 1
        || pending.expectedActivityClientGeneration != generation) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    try {
        const auto& selected = pending.squadRetirement;
        const auto prior = std::find_if(g_targets.begin(), g_targets.end(), [&](const Target& row) {
            return row.session == binding.sessionId && row.revision == binding.createdRevision
                   && row.generation == generation
                   && row.eligibility.squad.key == pending.target.registryKey
                   && row.eligibility.squad.index == pending.target.slotIndex;
        });
        const bool enabled = pending.kind == activity::host::ScriptableOverrideKind::squad
                             && selected.enabled && selected.squad.key == pending.target.registryKey
                             && selected.squad.index == pending.target.slotIndex
                             && selected.squad.type == 1;
        const bool unchanged =
            prior != g_targets.end() && enabled && prior->eligibility == selected;
        if (!unchanged) {
            g_store.invalidate_target(
                binding.sessionId,
                generation,
                {pending.target.registryKey, pending.target.slotIndex, pending.target.slotType});
            // A squad's body lives on the one link it was staged on, so this delivery re-pins the
            // squad: its rows on any link go. Other squads keep their links' rows. A release a
            // former link already captured stays, because it covers only that link's own trees.
            std::erase_if(g_targets, [&](const Target& row) {
                return row.session == binding.sessionId
                       && (row.revision != binding.createdRevision
                           || (row.eligibility.squad.key == pending.target.registryKey
                               && row.eligibility.squad.index == pending.target.slotIndex));
            });
            /** An active source cannot own more actor targets than native entity slots. */
            constexpr std::size_t kMaximumTargets =
                identities::kSourceCapacity * identities::kSlotCapacity;
            if (enabled && g_targets.size() < kMaximumTargets) {
                g_targets.push_back(
                    {binding.sessionId, binding.createdRevision, generation, generation, selected});
            }
        }
    } catch (...) {
        g_store.invalidate_target(
            binding.sessionId,
            generation,
            {pending.target.registryKey, pending.target.slotIndex, pending.target.slotType});
    }
    ReleaseSRWLockExclusive(&g_lock);
}
/**
 * A link that is not live re-pins nothing, so its squad's target moves as the squad's population
 * does, and returns to the delivering link once that link is live again.
 */
void follow_squad(const state::activity::SessionBinding& binding,
                  std::uint64_t generation,
                  identities::SquadReference squad,
                  std::span<const std::uint64_t> live) noexcept {
    // The delivering link changes only at a delivery, so a pass whose pin copy predates one cannot
    // move the target off a live delivering link.
    const auto followed = [&](const Target& row) noexcept {
        return std::find(live.begin(), live.end(), row.deliveredGeneration) != live.end()
                   ? row.deliveredGeneration
                   : generation;
    };
    const auto due = [&](const Target& row) noexcept {
        return row.session == binding.sessionId && row.revision == binding.createdRevision
               && row.eligibility.squad.key == squad.key
               && row.eligibility.squad.index == squad.index && row.generation != followed(row);
    };
    // Every admitted squad report asks, so the common answer is read without blocking capture.
    AcquireSRWLockShared(&g_lock);
    const bool moves = std::any_of(g_targets.begin(), g_targets.end(), due);
    ReleaseSRWLockShared(&g_lock);
    if (!moves) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (Target& row : g_targets) {
        if (due(row)) {
            row.generation = followed(row);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}
/** Only package-mapped, positively authored trees enter a captured release. */
void observe_abdication(const state::activity::SessionBinding& binding,
                        std::uint64_t generation,
                        std::uint8_t bubble,
                        const state::activity::bubble_authority::EntitySlotMask& mask) noexcept {
    identities::Source source{};
    std::vector<identities::Identity> rows;
    if (!snapshot(binding, generation, source, rows)) {
        report("capture_source", false, bubble);
        return;
    }
    const auto& destination = binding.destination;
    report_released(mask, rows);
    report_hierarchy_gaps(rows);
    if (destination.packageNameLength == 0
        || destination.packageNameLength > destination.packageName.size()) {
        return;
    }
    const std::string_view name(reinterpret_cast<const char*>(destination.packageName.data()),
                                destination.packageNameLength);
    policy::CellBubbles cells{};
    cells.fill(-1);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        std::uint8_t owner{};
        if (state::gameplay::entity_position_profiles::lookup_bubble(
                name, static_cast<std::uint16_t>(i), owner)) {
            cells[i] = owner;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    bool accepted = false;
    try {
        std::vector<policy::Eligibility> eligible;
        for (const auto& row : g_targets) {
            if (row.session == binding.sessionId && row.revision == binding.createdRevision
                && row.generation == generation) {
                eligible.push_back(row.eligibility);
            }
        }
        accepted = g_store.capture(source, bubble, mask, rows, eligible, cells);
    } catch (...) {}
    ReleaseSRWLockExclusive(&g_lock);
    report("capture", accepted, bubble);
}
void returned_slots(const state::activity::SessionBinding& binding,
                    std::uint64_t generation,
                    const state::activity::bubble_authority::EntitySlotMask& mask) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_store.returned_slots(binding.sessionId, generation, mask);
    g_propStore.returned_slots(binding.sessionId, generation, mask);
    ReleaseSRWLockExclusive(&g_lock);
}
/** A fresh atomic identity snapshot must still match each captured tree. */
bool prepare_retirement(const state::activity::SessionBinding& binding,
                        std::uint64_t generation,
                        std::uint8_t bubble,
                        RetirementPlan& output) noexcept {
    output = {};
    identities::Source source{};
    std::vector<identities::Identity> rows;
    if (!snapshot(binding, generation, source, rows)) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const bool ready = g_propStore.pending(source, bubble)
                           ? g_propStore.prepare(source, bubble, rows, output)
                           : g_store.prepare(source, bubble, rows, output);
    ReleaseSRWLockShared(&g_lock);
    report("prepare", ready, bubble, &output.entities);
    return ready;
}
/** The exact identity view remains pinned until its transport response has been copied. */
bool begin_retirement_publication(const state::activity::SessionBinding& binding,
                                  std::uint64_t generation,
                                  const RetirementPlan& plan,
                                  entity_identities::PublicationLease& lease) noexcept {
    if (lease.held()) {
        return false;
    }
    if (!plan.pending || plan.source.activitySessionId != binding.sessionId
        || plan.source.activityRevision != binding.createdRevision
        || plan.source.activityClientGeneration != generation
        || !state::activity::binding_matches(binding)) {
        return false;
    }
    std::vector<identities::Identity> rows;
    if (entity_identities::begin_publication(plan.source, rows, lease)
        != identities::Result::unchanged) {
        return false;
    }
    if (!state::gameplay::entity_object_types::enrich_snapshot(rows)) {
        lease.release();
        return false;
    }
    RetirementPlan current{};
    AcquireSRWLockShared(&g_lock);
    const auto& store = plan.placedProps ? g_propStore : g_store;
    const bool valid =
        store.prepare(plan.source, plan.bubble, rows, current) && current.source == plan.source
        && current.entities == plan.entities && current.lifetimes == plan.lifetimes
        && current.lifetimeCount == plan.lifetimeCount && current.revision == plan.revision
        && current.bubble == plan.bubble && current.placedProps == plan.placedProps;
    ReleaseSRWLockShared(&g_lock);
    if (!valid) {
        lease.release();
    }
    return valid;
}
/** A stale prepared retirement cannot enter a later transport publication. */
bool validate_retirement(const state::activity::SessionBinding& binding,
                         std::uint64_t generation,
                         const RetirementPlan& plan) noexcept {
    RetirementPlan current{};
    return plan.pending && prepare_retirement(binding, generation, plan.bubble, current)
           && current.source == plan.source && current.entities == plan.entities
           && current.lifetimes == plan.lifetimes && current.lifetimeCount == plan.lifetimeCount
           && current.revision == plan.revision && current.bubble == plan.bubble
           && current.placedProps == plan.placedProps;
}
/** Retires exact lifetimes only after their carrying publication and identity lease have ended. */
void commit_retirement(const RetirementPlan& plan) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    auto& store = plan.placedProps ? g_propStore : g_store;
    const bool stored = store.commit(plan);
    // A prop purge already published remains final if its script was cancelled after validation.
    const bool committed = stored
                           || (plan.placedProps && plan.pending && plan.lifetimeCount != 0
                               && plan.lifetimeCount <= plan.lifetimes.size());
    if (committed) {
        g_store.returned_slots(
            plan.source.activitySessionId, plan.source.activityClientGeneration, plan.entities);
        g_propStore.returned_slots(
            plan.source.activitySessionId, plan.source.activityClientGeneration, plan.entities);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (committed) {
        static_cast<void>(entity_identities::retire(plan.source, plan.retired_lifetimes()));
        static_cast<void>(peer::retire_entity_baselines(plan.source, plan.retired_lifetimes()));
    }
    report("commit", committed, plan.bubble, &plan.entities);
}
void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_store.reset();
    g_propStore.reset();
    g_targets.clear();
    g_propTransitions.clear();
    ReleaseSRWLockExclusive(&g_lock);
}
} // namespace sunrise::server::gameplay::squad_entity_retirement
