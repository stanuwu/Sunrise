#include "entity_identity.h"

#include <algorithm>
#include <bitset>
#include <new>

namespace sunrise::state::gameplay::entity_identity {
namespace {

/** Identity serials use eight bits and token incarnations use four. */
constexpr std::uint16_t kAllocationModulus = 256;
constexpr std::uint8_t kIncarnationLimit = 16, kTypeLimit = 4;
/** Wire flag four carries the terminal record's trailing state bit. */
constexpr std::uint16_t kTerminalFlag = 4;

bool valid_source(const Source& source) noexcept {
    return source.activitySessionId != 0 && source.activityRevision != 0
           && source.activityClientGeneration != 0 && source.groupSessionId != 0
           && source.peerGeneration != 0 && source.channelGeneration != 0
           && source.viewGeneration != 0;
}

bool valid_token(Token token) noexcept {
    return token.slot < kSlotCapacity && token.incarnation < kIncarnationLimit;
}

bool newer(std::uint16_t next, std::uint16_t previous, std::uint16_t modulus) noexcept {
    const auto distance = static_cast<std::uint16_t>((next + modulus - previous) % modulus);
    return distance != 0 && distance < modulus / 2;
}

/** @return True when the actor source matches what this observation may carry. */
bool valid_actor_source(const Observation& value) noexcept {
    const auto& source = value.actorSource;
    if (!source.known) {
        return !source.present;
    }
    if (value.type != 0 || value.action == Action::remove) {
        return false;
    }
    return !source.present
           || (source.key != 0 && source.key != 0xFFFFFFFFU && source.type <= 126
               && source.index <= 32767);
}

bool valid_observation(const Observation& value) noexcept {
    for (std::size_t pool = 0; pool < value.damage.levels.size(); ++pool) {
        if (value.damage.present[pool]
            && ((value.damage.levels[pool] >> kDamagePoolLevelBits) != 0 || value.type != 0
                || value.action == Action::remove)) {
            return false;
        }
    }
    return valid_token(value.token) && value.type < kTypeLimit && valid_actor_source(value)
           && (!value.hasPacketSequence || value.packetSequence < kPacketModulus)
           && (!value.anchorChanged || !value.anchorPresent || valid_token(value.anchor))
           && (value.action == Action::create || value.action == Action::update
               || value.action == Action::remove || value.action == Action::createAndRemove);
}

/** Compares retained state without transport observation timestamps. */
bool same_state(const Identity& left, const Identity& right) noexcept {
    return left.token == right.token && left.anchor == right.anchor
           && left.metadata == right.metadata && left.actorSource == right.actorSource
           && left.damage == right.damage && left.cell == right.cell
           && left.recordFlags == right.recordFlags
           && left.allocationSequence == right.allocationSequence
           && left.serialDomain == right.serialDomain
           && left.allocationDomain == right.allocationDomain
           && left.allocationEpoch == right.allocationEpoch
           && left.hasAllocationEpoch == right.hasAllocationEpoch && left.type == right.type
           && left.present == right.present && left.anchorKnown == right.anchorKnown
           && left.anchorPresent == right.anchorPresent
           && left.trailingState == right.trailingState;
}

/**
 * Stages one observation without changing the committed slot.
 * @param current Last identity or tombstone.
 * @param value Accepted wire observation.
 * @param next Receives the candidate, including conflict state.
 * @return The mutation result; stale and missing leave the slot unchanged.
 */
Result stage(const Identity& current,
             const Observation& value,
             Identity& next,
             std::uint64_t allocationDomain) noexcept {
    next = current;
    const bool ordinalOrder = current.known && value.hasPacketOrdinal && current.hasPacketOrdinal;
    if (ordinalOrder
        && (value.packetOrdinal < current.packetOrdinal
            || (value.packetOrdinal == current.packetOrdinal
                && value.packetRecordIndex < current.packetRecordIndex))) {
        return Result::stale;
    }
    if (!ordinalOrder && current.known && value.hasPacketSequence && current.hasPacketSequence
        && value.packetSequence != current.packetSequence
        && !newer(value.packetSequence, current.packetSequence, kPacketModulus)) {
        return Result::stale;
    }
    bool replacement = !current.known;
    if (value.action == Action::create || value.action == Action::createAndRemove) {
        const bool resetSerial =
            value.hasAllocationEpoch
            && (!current.hasAllocationEpoch || current.serialDomain != allocationDomain);
        if (value.hasAllocationEpoch && value.allocationSequence == 0) {
            return Result::stale;
        }
        if (current.known && !resetSerial) {
            const bool duplicate = current.token == value.token
                                   && current.allocationSequence == value.allocationSequence;
            if (duplicate) {
                if (!current.present) {
                    return Result::stale;
                }
                if (current.type != value.type || current.metadata != value.metadata) {
                    next.conflicted = true;
                    return Result::conflict;
                }
                if (current.conflicted) {
                    return Result::conflict;
                }
            } else {
                if (!newer(
                        value.allocationSequence, current.allocationSequence, kAllocationModulus)) {
                    return Result::stale;
                }
                replacement = true;
            }
        }
        const bool sameLiveType = current.known && current.present && !current.conflicted
                                  && current.token == value.token && current.type == value.type
                                  && current.metadata == value.metadata;
        const bool epochDuplicate =
            resetSerial && sameLiveType && current.allocationSequence == value.allocationSequence;
        const bool preserveLive = resetSerial && sameLiveType;
        replacement = replacement || (resetSerial && !epochDuplicate);
        if (replacement) {
            next = preserveLive ? current : Identity{};
            next.known = true;
            next.present = true;
            next.token = value.token;
            next.type = value.type;
            next.allocationSequence = value.allocationSequence;
            next.allocationEpoch = value.allocationEpoch;
            next.hasAllocationEpoch = value.hasAllocationEpoch;
            next.allocationDomain = value.hasAllocationEpoch ? allocationDomain : 0;
            next.metadata = value.metadata;
            next.anchorKnown = true;
        }
        next.serialDomain = value.hasAllocationEpoch ? allocationDomain : 0;
    } else {
        if (!current.known) {
            return Result::missing;
        }
        if (current.token != value.token) {
            return Result::stale;
        }
        if (current.conflicted) {
            return Result::conflict;
        }
        if (!current.present && value.action != Action::remove) {
            return Result::missing;
        }
        if (value.action == Action::remove) {
            next.present = false;
        }
    }
    for (std::size_t pool = 0; pool < value.damage.levels.size(); ++pool) {
        if (value.damage.present[pool]) {
            next.damage.levels[pool] = value.damage.levels[pool];
            next.damage.present[pool] = true;
        }
    }
    if (value.actorSource.known) {
        next.actorSource = value.actorSource.present ? value.actorSource : ActorSourceReference{};
        next.actorSource.known = true;
    }
    if (value.action == Action::createAndRemove) {
        next.present = false;
    }
    next.cell = value.cell;
    next.recordFlags = value.recordFlags;
    if ((value.recordFlags & kTerminalFlag) != 0) {
        next.trailingState = value.trailingState;
    }
    if (value.anchorChanged) {
        next.anchorKnown = true;
        next.anchorPresent = value.anchorPresent;
        next.anchor = value.anchorPresent ? value.anchor : Token{};
    }
    const bool samePacket =
        ordinalOrder ? value.packetOrdinal == current.packetOrdinal
                           && value.packetRecordIndex == current.packetRecordIndex
                     : current.known && value.hasPacketSequence && current.hasPacketSequence
                           && value.packetSequence == current.packetSequence;
    if (samePacket) {
        if (!same_state(current, next)) {
            next = current;
            next.conflicted = true;
            return Result::conflict;
        }
        if (value.hasPacketOrdinal && !current.hasPacketOrdinal) {
            next.hasPacketOrdinal = true;
            next.packetOrdinal = value.packetOrdinal;
            next.packetRecordIndex = value.packetRecordIndex;
        }
        return Result::unchanged;
    }
    next.tick = value.tick;
    if (value.hasPacketOrdinal) {
        next.hasPacketOrdinal = true;
        next.packetOrdinal = value.packetOrdinal;
        next.packetRecordIndex = value.packetRecordIndex;
    } else if (replacement && current.known) {
        next.hasPacketOrdinal = current.hasPacketOrdinal;
        next.packetOrdinal = current.packetOrdinal;
        next.packetRecordIndex = current.packetRecordIndex;
    }
    if (value.hasPacketSequence) {
        next.hasPacketSequence = true;
        next.packetSequence = value.packetSequence;
    } else if (replacement && current.known) {
        next.hasPacketSequence = current.hasPacketSequence;
        next.packetSequence = current.packetSequence;
    }
    if (value.action == Action::createAndRemove) {
        return Result::removed;
    }
    if (replacement) {
        return Result::created;
    }
    if (value.action == Action::remove) {
        return current.present ? Result::removed : Result::unchanged;
    }
    if (same_state(current, next) && next.hasPacketSequence == current.hasPacketSequence
        && next.packetSequence == current.packetSequence
        && next.hasPacketOrdinal == current.hasPacketOrdinal
        && next.packetOrdinal == current.packetOrdinal
        && next.packetRecordIndex == current.packetRecordIndex) {
        return Result::unchanged;
    }
    return Result::updated;
}
} // namespace

/**
 * Delivered retirements leave tombstones without erasing a newer allocation.
 * @param source
 * Exact admitted source generation.
 * @param lifetimes Allocations named by the published
 * retirement.
 * @return Number of live identities retired; invalid inputs change nothing.
 */
std::size_t Store::retire(const Source& source,
                          std::span<const RetiredLifetime> lifetimes) noexcept {
    if (!valid_source(source) || lifetimes.size() > kSlotCapacity
        || std::any_of(lifetimes.begin(), lifetimes.end(), [](const auto& lifetime) {
               return !valid_token(lifetime.token);
           })) {
        return 0;
    }
    auto partition = std::find_if(partitions_.begin(), partitions_.end(), [&](const auto& value) {
        return value.occupied && value.source == source;
    });
    if (partition == partitions_.end()) {
        return 0;
    }
    std::size_t retired = 0;
    for (const auto& lifetime : lifetimes) {
        auto& current = partition->slots[lifetime.token.slot];
        if (!current.known || !current.present || current.token != lifetime.token
            || current.allocationSequence != lifetime.allocationSequence
            || current.allocationEpoch != lifetime.allocationEpoch
            || current.allocationDomain != lifetime.allocationDomain) {
            continue;
        }
        current.present = false;
        current.revision = ++revision_;
        ++retired;
    }
    return retired;
}

/**
 * Commits a bounded identity mutation after validation and allocation succeed.
 * @param source Exact admitted source generation.
 * @param observation Accepted wire record.
 * @return The committed result, or the reason the record was refused.
 */
Result Store::observe(const Source& source, const Observation& observation) noexcept {
    if (!valid_source(source) || !valid_observation(observation)
        || (observation.hasAllocationEpoch && observation.allocationDomain == 0)) {
        return Result::invalid;
    }
    auto partition = std::find_if(partitions_.begin(), partitions_.end(), [&](const auto& value) {
        return value.occupied && value.source == source;
    });
    if (partition != partitions_.end() && partition->hasAllocationEpoch
        && (!observation.hasAllocationEpoch
            || observation.allocationEpoch != partition->allocationEpoch
            || observation.allocationDomain != partition->allocationDomain)) {
        return Result::stale;
    }
    const Identity empty{};
    const Identity& current =
        partition == partitions_.end() ? empty : partition->slots[observation.token.slot];
    if (observation.actorSource.known && observation.action != Action::create
        && observation.action != Action::createAndRemove && current.known && current.type != 0) {
        return Result::invalid;
    }
    Identity next{};
    const Result result = stage(current, observation, next, observation.allocationDomain);
    if (result == Result::missing || result == Result::stale
        || (result == Result::unchanged && next.hasPacketSequence == current.hasPacketSequence
            && next.packetSequence == current.packetSequence
            && next.hasPacketOrdinal == current.hasPacketOrdinal
            && next.packetOrdinal == current.packetOrdinal
            && next.packetRecordIndex == current.packetRecordIndex)
        || (result == Result::conflict && current.conflicted)) {
        return result;
    }
    if (partition == partitions_.end()) {
        partition = std::find_if(partitions_.begin(), partitions_.end(), [](const auto& value) {
            return !value.occupied;
        });
        if (partition == partitions_.end()) {
            return Result::capacity;
        }
        try {
            std::vector<Identity> slots(kSlotCapacity);
            partition->slots = std::move(slots);
        } catch (const std::bad_alloc&) {
            return Result::capacity;
        }
        partition->source = source;
        partition->occupied = true;
    }
    if (observation.hasAllocationEpoch) {
        partition->allocationDomain = observation.allocationDomain;
        partition->hasAllocationEpoch = true;
        partition->allocationEpoch = observation.allocationEpoch;
    }
    next.revision = ++revision_;
    partition->slots[observation.token.slot] = next;
    return result;
}

/** A packet stages all records before terminal roots retire their known descendants. */
Result Store::observe_batch(const Source& source,
                            std::span<const Observation> observations,
                            std::span<Result> results) noexcept {
    if (!valid_source(source) || observations.empty()
        || observations.size() > kObservationBatchCapacity
        || results.size() < observations.size()) {
        return Result::invalid;
    }
    auto partition = std::find_if(partitions_.begin(), partitions_.end(), [&](const auto& value) {
        return value.occupied && value.source == source;
    });
    const auto& packetEpoch = observations.front();
    if (partition != partitions_.end() && partition->hasAllocationEpoch
        && (!packetEpoch.hasAllocationEpoch
            || packetEpoch.allocationEpoch != partition->allocationEpoch
            || packetEpoch.allocationDomain != partition->allocationDomain)) {
        return Result::stale;
    }
    if (packetEpoch.hasAllocationEpoch && packetEpoch.allocationDomain == 0) {
        return Result::invalid;
    }
    struct Change {
        std::uint16_t slot{};
        Identity value{};
    };
    std::array<Change, kBatchChangeCapacity> changes{};
    std::array<Result, kObservationBatchCapacity> stagedResults{};
    std::size_t changeCount = 0;
    const Identity empty{};
    const auto current = [&](std::uint16_t slot) -> const Identity& {
        for (std::size_t index = 0; index < changeCount; ++index) {
            if (changes[index].slot == slot) {
                return changes[index].value;
            }
        }
        return partition == partitions_.end() ? empty : partition->slots[slot];
    };
    const auto assign = [&](std::uint16_t slot, const Identity& value) {
        std::size_t index = 0;
        for (; index < changeCount && changes[index].slot != slot; ++index) {}
        if (index == changeCount) {
            if (changeCount == changes.size()) {
                return false;
            }
            changes[changeCount++].slot = slot;
        }
        changes[index].value = value;
        return true;
    };
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& observation = observations[index];
        if (!valid_observation(observation)) {
            return Result::invalid;
        }
        const auto& packet = observations.front();
        if (observation.hasAllocationEpoch != packet.hasAllocationEpoch
            || observation.allocationEpoch != packet.allocationEpoch
            || observation.allocationDomain != packet.allocationDomain
            || observation.hasPacketSequence != packet.hasPacketSequence
            || observation.packetSequence != packet.packetSequence
            || observation.hasPacketOrdinal != packet.hasPacketOrdinal
            || observation.packetOrdinal != packet.packetOrdinal
            || (index != 0
                && observation.packetRecordIndex <= observations[index - 1].packetRecordIndex)) {
            return Result::invalid;
        }
        const auto& before = current(observation.token.slot);
        Observation value = observation;
        if (value.action == Action::createAndRemove) {
            value.action = Action::create;
        }
        if (value.action == Action::remove && before.present) {
            value.action = Action::update;
        }
        if (value.actorSource.known && value.action != Action::create && before.known
            && before.type != 0) {
            return Result::invalid;
        }
        Identity next{};
        const auto result = stage(before, value, next, value.allocationDomain);
        if (result == Result::missing || result == Result::stale || result == Result::conflict
            || result == Result::invalid || result == Result::capacity) {
            return result;
        }
        if (!assign(observation.token.slot, next)) {
            return Result::capacity;
        }
        stagedResults[index] = result;
    }
    // Match the receiver baseline's anchor pass before recursive removal.
    const auto recorded = [&](Token token) {
        return std::any_of(observations.begin(), observations.end(), [&](const auto& row) {
            return row.token == token;
        });
    };
    // Only an anchored identity can detach. An unstaged slot still reads as the partition holds
    // it, so most of the table is rejected before any staged-change or record search.
    std::bitset<kSlotCapacity> stagedSlots{};
    for (std::size_t index = 0; index < changeCount; ++index) {
        stagedSlots.set(changes[index].slot);
    }
    for (std::size_t slot = 0; slot < kSlotCapacity; ++slot) {
        if (!stagedSlots.test(slot)) {
            if (partition == partitions_.end()) {
                continue;
            }
            const Identity& retained = partition->slots[slot];
            if (!retained.known || !retained.present || !retained.anchorPresent) {
                continue;
            }
        }
        auto child = current(static_cast<std::uint16_t>(slot));
        if (!child.known || !child.present || !child.anchorPresent || recorded(child.token)
            || !recorded(child.anchor)) {
            continue;
        }
        child.anchor = {};
        child.anchorKnown = true;
        child.anchorPresent = false;
        const auto& order = observations.back();
        if (child.hasPacketOrdinal && order.hasPacketOrdinal
            && child.packetOrdinal > order.packetOrdinal) {
            return Result::stale;
        }
        child.tick = order.tick;
        if (order.hasPacketOrdinal) {
            child.hasPacketOrdinal = true;
            child.packetOrdinal = order.packetOrdinal;
            child.packetRecordIndex = order.packetRecordIndex;
        }
        if (order.hasPacketSequence) {
            child.hasPacketSequence = true;
            child.packetSequence = order.packetSequence;
        }
        if (!assign(static_cast<std::uint16_t>(slot), child)) {
            return Result::capacity;
        }
    }
    std::array<Token, kBatchChangeCapacity> terminals{};
    std::size_t terminalCount = 0;
    const auto add_terminal = [&](Token token) {
        for (std::size_t index = 0; index < terminalCount; ++index) {
            if (terminals[index] == token) {
                return true;
            }
        }
        if (terminalCount == terminals.size()) {
            return false;
        }
        terminals[terminalCount++] = token;
        return true;
    };
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& observation = observations[index];
        if (observation.action == Action::remove || observation.action == Action::createAndRemove) {
            if (!add_terminal(observation.token)) {
                return Result::capacity;
            }
            stagedResults[index] = Result::removed;
        }
    }
    const auto& order = observations.back();
    for (std::size_t index = 0; index < terminalCount; ++index) {
        const auto token = terminals[index];
        const auto& before = current(token.slot);
        if (!before.known || !before.present || before.token != token) {
            continue;
        }
        if (before.hasPacketOrdinal && order.hasPacketOrdinal
            && before.packetOrdinal > order.packetOrdinal) {
            return Result::stale;
        }
        for (std::size_t slot = 0; slot < kSlotCapacity; ++slot) {
            const auto& child = current(static_cast<std::uint16_t>(slot));
            if (child.known && child.present && child.anchorPresent && child.anchor == token
                && !add_terminal(child.token)) {
                return Result::capacity;
            }
        }
        Identity retired = before;
        retired.present = false;
        retired.tick = order.tick;
        if (order.hasPacketOrdinal) {
            retired.hasPacketOrdinal = true;
            retired.packetOrdinal = order.packetOrdinal;
            retired.packetRecordIndex = order.packetRecordIndex;
        }
        if (order.hasPacketSequence) {
            retired.hasPacketSequence = true;
            retired.packetSequence = order.packetSequence;
        }
        if (!assign(token.slot, retired)) {
            return Result::capacity;
        }
    }
    if (partition == partitions_.end()) {
        partition = std::find_if(partitions_.begin(), partitions_.end(), [](const auto& value) {
            return !value.occupied;
        });
        if (partition == partitions_.end()) {
            return Result::capacity;
        }
        try {
            partition->slots.resize(kSlotCapacity);
        } catch (const std::bad_alloc&) {
            return Result::capacity;
        }
        partition->source = source;
        partition->occupied = true;
    }
    if (packetEpoch.hasAllocationEpoch) {
        partition->allocationDomain = packetEpoch.allocationDomain;
        partition->hasAllocationEpoch = true;
        partition->allocationEpoch = packetEpoch.allocationEpoch;
    }
    for (std::size_t index = 0; index < changeCount; ++index) {
        changes[index].value.revision = ++revision_;
        partition->slots[changes[index].slot] = changes[index].value;
    }
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& value = partition->slots[observations[index].token.slot];
        if (value.known && !value.present && value.token == observations[index].token) {
            stagedResults[index] = Result::removed;
        }
    }
    std::copy_n(stagedResults.begin(), observations.size(), results.begin());
    return Result::updated;
}

/**
 * Returns identity evidence without granting any authority over the entity.
 * @param source
 * Exact admitted source generation.
 * @param token Expected incarnation of the slot.
 * @param output Receives the known identity or tombstone; cleared for a missing source.
 * @return Unchanged for live identity, removed for a tombstone, or its refusal state.
 */
Result Store::lookup(const Source& source, Token token, Identity& output) const noexcept {
    output = {};
    if (!valid_source(source) || !valid_token(token)) {
        return Result::invalid;
    }
    const auto partition =
        std::find_if(partitions_.begin(), partitions_.end(), [&](const auto& row) {
            return row.occupied && row.source == source;
        });
    if (partition == partitions_.end()) {
        return Result::missing;
    }
    output = partition->slots[token.slot];
    if (!output.known) {
        return Result::missing;
    }
    if (output.token != token) {
        return Result::stale;
    }
    if (output.conflicted) {
        return Result::conflict;
    }
    return output.present ? Result::unchanged : Result::removed;
}

/**
 * Copies every slot from one source, including tombstones and conflicts.
 * @param source Exact admitted source generation.
 * @param output Receives all slots, or is cleared on failure.
 * @return Unchanged for a copied source, missing, invalid, or capacity on failure.
 */
Result Store::snapshot_source(const Source& source, std::vector<Identity>& output) const noexcept {
    output.clear();
    if (!valid_source(source)) {
        return Result::invalid;
    }
    const auto partition =
        std::find_if(partitions_.begin(), partitions_.end(), [&](const auto& row) {
            return row.occupied && row.source == source;
        });
    if (partition == partitions_.end()) {
        return Result::missing;
    }
    try {
        output = partition->slots;
    } catch (const std::bad_alloc&) {
        output.clear();
        return Result::capacity;
    }
    return Result::unchanged;
}

/**
 * Visits every slot from one source in slot order, including tombstones and conflicts.
 * @param source Exact admitted source generation.
 * @param visitor Receives each slot; returning false ends the walk early.
 * @param context Caller state passed unchanged to the visitor.
 * @return Unchanged for a visited source, missing, or invalid for a bad source or visitor.
 */
Result
Store::visit_source(const Source& source, SlotVisitor visitor, void* context) const noexcept {
    if (!valid_source(source) || visitor == nullptr) {
        return Result::invalid;
    }
    const auto partition =
        std::find_if(partitions_.begin(), partitions_.end(), [&](const auto& row) {
            return row.occupied && row.source == source;
        });
    if (partition == partitions_.end()) {
        return Result::missing;
    }
    for (const Identity& slot : partition->slots) {
        if (!visitor(context, slot)) {
            break;
        }
    }
    return Result::unchanged;
}

/**
 * Counts all matching sources even when the caller's output is smaller.
 * @param activitySessionId Activity owning the sources.
 * @param activityRevision Exact activity generation.
 * @param output Receives the bounded prefix of matching sources.
 * @return Total matching source count.
 */
std::size_t Store::sources(std::uint64_t activitySessionId,
                           std::uint64_t activityRevision,
                           std::span<Source> output) const noexcept {
    std::size_t count = 0;
    for (const Partition& partition : partitions_) {
        if (partition.occupied && partition.source.activitySessionId == activitySessionId
            && partition.source.activityRevision == activityRevision) {
            if (count < output.size()) {
                output[count] = partition.source;
            }
            ++count;
        }
    }
    return count;
}

/** Changes only the serial domain of the exact admitted source. */
bool Store::advance_epoch(const Source& source,
                          std::uint8_t expected,
                          std::uint8_t next,
                          std::uint64_t nextDomain) noexcept {
    if (!valid_source(source) || nextDomain == 0
        || next != static_cast<std::uint8_t>(expected + 1U)) {
        return false;
    }
    for (auto& partition : partitions_) {
        if (partition.occupied && partition.source == source) {
            if (partition.hasAllocationEpoch
                && (partition.allocationEpoch != expected
                    || partition.allocationDomain != nextDomain - 1)) {
                return false;
            }
            partition.allocationDomain = nextDomain;
            partition.allocationEpoch = next;
            partition.hasAllocationEpoch = true;
            return true;
        }
    }
    return false;
}

void Store::reset_group(std::uint64_t groupSessionId) noexcept {
    for (Partition& partition : partitions_) {
        if (partition.occupied && partition.source.groupSessionId == groupSessionId) {
            partition = {};
        }
    }
}

void Store::reset_source(const Source& source) noexcept {
    for (Partition& partition : partitions_) {
        if (partition.occupied && partition.source == source) {
            partition = {};
        }
    }
}

void Store::reset() noexcept {
    for (Partition& partition : partitions_) {
        partition = {};
    }
    revision_ = 0;
}

} // namespace sunrise::state::gameplay::entity_identity
