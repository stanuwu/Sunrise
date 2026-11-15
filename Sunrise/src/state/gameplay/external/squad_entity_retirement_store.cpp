#include <algorithm>

#include "squad_entity_retirement.h"

namespace sunrise::state::gameplay::squad_entity_retirement {
namespace {
using middleware::bap::activity_message::entity_slots::has_slot;
void set(Mask& mask, std::size_t slot) {
    mask[slot / 8] |= std::byte(1U << (slot % 8));
}
bool any(const Mask& mask) {
    return std::any_of(mask.begin(), mask.end(), [](auto value) { return value != std::byte{}; });
}
bool same(const identities::Identity& a, const identities::Identity& b) {
    return a.known && b.known && a.present && b.present && !a.conflicted && !b.conflicted
           && a.token == b.token && a.allocationSequence == b.allocationSequence
           && a.allocationEpoch == b.allocationEpoch && a.allocationDomain == b.allocationDomain
           && a.hasAllocationEpoch == b.hasAllocationEpoch && a.type == b.type
           && a.metadata == b.metadata && a.actorSource == b.actorSource && a.cell == b.cell
           && a.anchorKnown && b.anchorKnown && a.anchorPresent == b.anchorPresent
           && (!a.anchorPresent || a.anchor == b.anchor);
}
/** @return True for the slot types a spawned actor names as its source. */
bool actor_source(std::uint8_t type) {
    return type == kSquadSourceType || type == kCombatantSourceType;
}
/** An entity of the eligible class from its exact source: an actor, or an authored object entry. */
bool sourced(const identities::Identity& row, const Eligibility& eligibility) {
    const auto type = row.actorSource.type;
    return row.known && row.present && !row.conflicted && row.type == 0 && row.metadata.hasRsat
           && row.metadata.rsatTag == eligibility.rsatTag && !row.metadata.hasPlayerBroadcast
           && row.actorSource.known && row.actorSource.present
           && (actor_source(type) || type == kObjectSourceType)
           && row.actorSource.key == eligibility.squad.key
           && row.actorSource.type == eligibility.squad.type
           && row.actorSource.index == eligibility.squad.index;
}
/** All-one bits name no installed class. */
constexpr std::uint32_t kAbsentRsatTag = 0xFFFFFFFFU;
/** A prop needs a positive class and type without any player or actor ownership. */
bool prop(const identities::Identity& row) {
    return row.known && row.present && !row.conflicted && row.type == 0 && row.metadata.hasRsat
           && row.metadata.rsatTag != 0 && row.metadata.rsatTag != kAbsentRsatTag
           && row.metadata.hasObjectType
           && std::find(
                  kRetirablePropTypes.begin(), kRetirablePropTypes.end(), row.metadata.objectType)
                  != kRetirablePropTypes.end()
           && !row.metadata.hasPlayerBroadcast && !row.metadata.hasSquad
           && !row.actorSource.present;
}
bool root_matches(const identities::Identity& row, const Eligibility& eligibility) {
    return eligibility.placedProp ? prop(row) && row.metadata.rsatTag == eligibility.rsatTag
                                        && row.metadata.objectType == eligibility.objectType
                                  : sourced(row, eligibility);
}
/**
 * A child released under a retired root leaves with it, such as an actor's weapon, shield or
 * hop-on. The client's release is its own dependency closure; a player's object or another squad's
 * member never belongs to the tree.
 */
bool dependent(const identities::Identity& row, const Eligibility& eligibility) {
    if (row.type != 0 || !row.metadata.hasRsat || row.metadata.hasPlayerBroadcast
        || !row.anchorKnown || !row.anchorPresent) {
        return false;
    }
    // Another squad's or combatant's actor never joins; a weapon or hop-on (another source) does.
    const auto& source = row.actorSource;
    return !source.known || !source.present || !actor_source(source.type)
           || (source.type == eligibility.squad.type && source.key == eligibility.squad.key
               && source.index == eligibility.squad.index);
}
/** Every live anchor must resolve before a root's complete child set is trustworthy. */
bool hierarchy(std::span<const identities::Identity> rows) {
    if (rows.size() != identities::kSlotCapacity) {
        return false;
    }
    for (std::size_t slot = 0; slot < rows.size(); ++slot) {
        const auto& row = rows[slot];
        if (!row.present) {
            continue;
        }
        if (!row.known || row.conflicted || !row.anchorKnown || row.token.slot != slot) {
            return false;
        }
        if (row.anchorPresent
            && (row.anchor.slot >= rows.size() || !rows[row.anchor.slot].present
                || rows[row.anchor.slot].token != row.anchor)) {
            return false;
        }
    }
    return true;
}
/** Unknown roles refuse the complete tree. */
bool closure(identities::Token root,
             const Eligibility& eligibility,
             const Mask& released,
             std::span<const identities::Identity> rows,
             Mask& output) {
    output = {};
    if (root.slot >= rows.size() || rows[root.slot].token != root
        || !root_matches(rows[root.slot], eligibility) || rows[root.slot].anchorPresent
        || !has_slot(released, root.slot)) {
        return false;
    }
    set(output, root.slot);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& row : rows) {
            if (!row.present || !row.anchorPresent || !has_slot(output, row.anchor.slot)
                || has_slot(output, row.token.slot)) {
                continue;
            }
            const bool squad =
                row.type == 1 && row.metadata.hasSquad && row.metadata.squad == eligibility.squad;
            const bool eligible =
                eligibility.placedProp
                    ? prop(row)
                    : sourced(row, eligibility) || dependent(row, eligibility) || squad;
            if (!has_slot(released, row.token.slot) || !eligible) {
                return false;
            }
            set(output, row.token.slot);
            changed = true;
        }
    }
    return true;
}
} // namespace
/** Strict captures refuse every matched prop tree that cannot be retired in full. */
bool Store::capture(const identities::Source& source,
                    std::uint8_t bubble,
                    const Mask& released,
                    std::span<const identities::Identity> rows,
                    std::span<const Eligibility> eligibility,
                    const CellBubbles& cells,
                    bool requireComplete) noexcept {
    try {
        if (source.activitySessionId == 0 || source.activityClientGeneration == 0 || bubble >= 64
            || !hierarchy(rows)) {
            return false;
        }
        Release next;
        next.source = source;
        next.bubble = bubble;
        next.requireComplete = requireComplete;
        for (const auto& row : rows) {
            if (!row.present || row.anchorPresent || !has_slot(released, row.token.slot)) {
                continue;
            }
            const Eligibility* selected = nullptr;
            for (const auto& candidate : eligibility) {
                if (candidate.enabled && candidate.bubble == bubble
                    && root_matches(row, candidate)) {
                    if (selected) {
                        if (requireComplete && (selected->placedProp || candidate.placedProp)) {
                            return false;
                        }
                        selected = nullptr;
                        break;
                    }
                    selected = &candidate;
                }
            }
            if (!selected) {
                continue;
            }
            if (row.cell >= cells.size() || cells[row.cell] != bubble) {
                if (requireComplete && selected->placedProp) {
                    return false;
                }
                continue;
            }
            // Each member is its own tree, so a squad of several retires member by member.
            Release::Group group;
            group.root = row.token;
            group.eligibility = *selected;
            if (!closure(row.token, *selected, released, rows, group.entities)) {
                if (requireComplete && selected->placedProp) {
                    return false;
                }
                continue;
            }
            bool valid = true;
            for (std::size_t slot = 0; slot < rows.size(); ++slot) {
                if (has_slot(group.entities, slot)) {
                    const auto& member = rows[slot];
                    if (member.cell >= cells.size() || cells[member.cell] != bubble) {
                        valid = false;
                        break;
                    }
                    group.captured.push_back(member);
                }
            }
            if (requireComplete && selected->placedProp
                && (!valid || group.captured.size() > RetirementPlan::kLifetimeCapacity)) {
                return false;
            }
            if (valid) {
                next.groups.push_back(std::move(group));
            }
        }
        for (const auto& old : releases_) {
            if (old.source != source || old.bubble != bubble) {
                continue;
            }
            for (const auto& group : old.groups) {
                bool overlaps = false;
                for (std::size_t i = 0; i < released.size(); ++i) {
                    if ((released[i] & group.entities[i]) != std::byte{}) {
                        overlaps = true;
                        break;
                    }
                }
                if (overlaps) {
                    continue;
                }
                Mask current{};
                if (!closure(group.root, group.eligibility, group.entities, rows, current)
                    || current != group.entities) {
                    continue;
                }
                if (std::all_of(
                        group.captured.begin(), group.captured.end(), [&](const auto& before) {
                            return same(before, rows[before.token.slot]);
                        })) {
                    next.groups.push_back(group);
                }
            }
        }
        if (next.groups.empty() && requireComplete) {
            return false;
        }
        /** Pending releases are bounded by the native source and bubble domains. */
        constexpr std::size_t kMaximumReleases = identities::kSourceCapacity * 64;
        const auto replacing =
            std::count_if(releases_.begin(), releases_.end(), [&](const auto& old) {
                return old.source.activitySessionId == source.activitySessionId
                       && old.source.activityClientGeneration == source.activityClientGeneration
                       && (old.source != source || old.bubble == bubble);
            });
        if (!next.groups.empty()
            && releases_.size() - static_cast<std::size_t>(replacing) >= kMaximumReleases) {
            return false;
        }
        // Reserve before changing retained releases; allocation failure leaves them intact.
        releases_.reserve(releases_.size() + 1);
        for (auto& previous : releases_) {
            if (previous.source != source || previous.bubble == bubble) {
                continue;
            }
            const auto count = previous.groups.size();
            std::erase_if(previous.groups, [&](const Release::Group& group) {
                for (std::size_t i = 0; i < released.size(); ++i) {
                    if ((released[i] & group.entities[i]) != std::byte{}) {
                        if (previous.requireComplete) {
                            previous.invalidated = true;
                            return false;
                        }
                        return true;
                    }
                }
                return false;
            });
            if (previous.groups.size() != count || previous.invalidated) {
                previous.revision = ++revision_;
            }
        }
        std::erase_if(releases_, [&](const Release& old) {
            return old.groups.empty()
                   || (old.source.activitySessionId == source.activitySessionId
                       && old.source.activityClientGeneration == source.activityClientGeneration
                       && (old.source != source || old.bubble == bubble));
        });
        if (next.groups.empty()) {
            return false;
        }
        next.revision = ++revision_;
        releases_.push_back(std::move(next));
        return true;
    } catch (...) {
        return false;
    }
}
/** Preparation compares the release's lifetime and full tree against a fresh atomic snapshot. */
bool Store::prepare(const identities::Source& source,
                    std::uint8_t bubble,
                    std::span<const identities::Identity> rows,
                    RetirementPlan& output) const noexcept {
    output = {};
    if (!hierarchy(rows)) {
        return false;
    }
    for (const auto& release : releases_) {
        if (release.source != source || release.bubble != bubble) {
            continue;
        }
        if (release.invalidated) {
            return false;
        }
        for (const auto& group : release.groups) {
            // A member the client already rebuilt is a new root outside every captured tree.
            Mask current{};
            if (!closure(group.root, group.eligibility, group.entities, rows, current)
                || current != group.entities) {
                if (release.requireComplete && group.eligibility.placedProp) {
                    output = {};
                    return false;
                }
                continue;
            }
            bool valid = true;
            for (const auto& before : group.captured) {
                if (!same(before, rows[before.token.slot])) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                if (group.captured.size() > output.lifetimes.size()) {
                    output = {};
                    return false;
                }
                // A publication contains whole trees, even when the remaining space is too small.
                if (group.captured.size() > output.lifetimes.size() - output.lifetimeCount) {
                    continue;
                }
                for (std::size_t index = 0; index < output.entities.size(); ++index) {
                    output.entities[index] |= group.entities[index];
                }
                for (const auto& member : group.captured) {
                    output.lifetimes[output.lifetimeCount++] = {member.token,
                                                                member.allocationSequence,
                                                                member.allocationEpoch,
                                                                member.allocationDomain};
                }
            } else if (release.requireComplete && group.eligibility.placedProp) {
                output = {};
                return false;
            }
        }
        if (!any(output.entities)) {
            return false;
        }
        output.source = source;
        output.bubble = bubble;
        output.revision = release.revision;
        output.pending = true;
        output.placedProps = release.requireComplete;
        return true;
    }
    return false;
}
/** A discarded or obsolete plan consumes no released entity. */
bool Store::commit(const RetirementPlan& plan) noexcept {
    if (!plan.pending || !any(plan.entities) || plan.lifetimeCount == 0
        || plan.lifetimeCount > plan.lifetimes.size()) {
        return false;
    }
    for (auto& release : releases_) {
        if (release.source == plan.source && release.bubble == plan.bubble
            && release.revision == plan.revision && !release.invalidated) {
            Mask covered{};
            for (const auto& group : release.groups) {
                bool complete = true;
                for (std::size_t i = 0; i < plan.entities.size(); ++i) {
                    if ((group.entities[i] & plan.entities[i]) != group.entities[i]) {
                        complete = false;
                        break;
                    }
                }
                if (complete) {
                    for (std::size_t i = 0; i < covered.size(); ++i) {
                        covered[i] |= group.entities[i];
                    }
                }
            }
            if (covered != plan.entities) {
                return false;
            }
            Mask exact{};
            for (const auto& lifetime : plan.retired_lifetimes()) {
                if (lifetime.token.slot >= identities::kSlotCapacity) {
                    return false;
                }
                const auto byte = lifetime.token.slot / 8;
                const auto bit = static_cast<std::byte>(1U << (lifetime.token.slot % 8));
                if ((plan.entities[byte] & bit) == std::byte{}
                    || (exact[byte] & bit) != std::byte{}) {
                    return false;
                }
                bool found = false;
                for (const auto& group : release.groups) {
                    for (const auto& row : group.captured) {
                        if (row.token == lifetime.token
                            && row.allocationSequence == lifetime.allocationSequence
                            && row.allocationEpoch == lifetime.allocationEpoch
                            && row.allocationDomain == lifetime.allocationDomain) {
                            found = true;
                        }
                    }
                }
                if (!found) {
                    return false;
                }
                exact[byte] |= bit;
            }
            if (exact != plan.entities) {
                return false;
            }
            std::erase_if(release.groups, [&](const Release::Group& group) {
                for (std::size_t i = 0; i < plan.entities.size(); ++i) {
                    if ((group.entities[i] & plan.entities[i]) != group.entities[i]) {
                        return false;
                    }
                }
                return true;
            });
            release.revision = ++revision_;
            return true;
        }
    }
    return false;
}
/** Returning any member invalidates its entire captured tree. */
void Store::returned_slots(std::uint64_t session,
                           std::uint64_t generation,
                           const Mask& mask) noexcept {
    for (auto& release : releases_) {
        if (release.source.activitySessionId == session
            && release.source.activityClientGeneration == generation) {
            std::erase_if(release.groups, [&](const Release::Group& group) {
                for (std::size_t i = 0; i < mask.size(); ++i) {
                    if ((group.entities[i] & mask[i]) != std::byte{}) {
                        if (release.requireComplete) {
                            release.invalidated = true;
                            return false;
                        }
                        return true;
                    }
                }
                return false;
            });
            release.revision = ++revision_;
        }
    }
}
/** Changing one authored target cannot consume another squad's pending release. */
void Store::invalidate_target(std::uint64_t session,
                              std::uint64_t generation,
                              identities::SquadReference target) noexcept {
    for (auto& release : releases_) {
        if (release.source.activitySessionId == session
            && release.source.activityClientGeneration == generation) {
            const auto old = release.groups.size();
            std::erase_if(release.groups, [&](const Release::Group& group) {
                return !group.eligibility.placedProp && group.eligibility.squad == target;
            });
            if (release.groups.size() != old) {
                release.revision = ++revision_;
            }
        }
    }
}
bool Store::pending(const identities::Source& source, std::uint8_t bubble) const noexcept {
    return std::any_of(releases_.begin(), releases_.end(), [&](const Release& release) {
        return release.source == source && release.bubble == bubble && !release.groups.empty();
    });
}
std::optional<std::uint8_t> Store::pending_bubble(const identities::Source& source,
                                                  std::uint8_t excluded) const noexcept {
    const auto release = std::find_if(releases_.begin(), releases_.end(), [&](const Release& row) {
        return row.source == source && row.bubble != excluded && !row.groups.empty();
    });
    return release != releases_.end() ? std::optional(release->bubble) : std::nullopt;
}
/** Cancelling a transition invalidates every outstanding plan for its captured source. */
void Store::invalidate_source(const identities::Source& source, std::uint8_t bubble) noexcept {
    std::erase_if(releases_, [&](const Release& release) {
        return release.source == source && release.bubble == bubble;
    });
    ++revision_;
}
void Store::reset() noexcept {
    releases_.clear();
    ++revision_;
}
} // namespace sunrise::state::gameplay::squad_entity_retirement
