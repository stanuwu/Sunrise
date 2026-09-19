#include <array>
#include <bitset>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "composite_entity_codec_internal.h"

namespace sunrise::middleware::gameplay::external {
namespace {

namespace format = state::activity_sdk::format;
namespace bits = middleware::encoding::bits;

/** Compares every field protected by a staged registry commit. */
[[nodiscard]] bool same_slot(const EntityBaselineSlot& left,
                             const EntityBaselineSlot& right) noexcept {
    return left.allocationEpoch == right.allocationEpoch
           && left.hasAllocationEpoch == right.hasAllocationEpoch
           && left.allocationDomain == right.allocationDomain
           && left.serialDomain == right.serialDomain && left.rsatTag == right.rsatTag
           && left.allocationSequence == right.allocationSequence
           && left.incarnation == right.incarnation && left.type == right.type
           && left.occupied == right.occupied && left.known == right.known
           && left.hasPacketSequence == right.hasPacketSequence
           && left.packetSequence == right.packetSequence
           && left.packetOrdinal == right.packetOrdinal
           && left.sobjectPlacement == right.sobjectPlacement
           && left.anchorPresent == right.anchorPresent && left.anchorOrder == right.anchorOrder
           && left.anchor.slot == right.anchor.slot
           && left.anchor.incarnation == right.anchor.incarnation;
}

/** Compares wrapping allocation sequences within the unambiguous half-range. */
[[nodiscard]] bool serial_is_newer(std::uint8_t candidate, std::uint8_t current) noexcept {
    /** Half of the byte sequence space. A larger gap cannot be ordered. */
    constexpr std::uint8_t kOrderableDistance = 0x80U;
    const std::uint8_t distance = static_cast<std::uint8_t>(candidate - current);
    return distance != 0 && distance < kOrderableDistance;
}

} // namespace

namespace detail {

/** Finds or creates one bounded session without evicting another session. */
[[nodiscard]] CompositeEntitySession*
session(CompositeEntitySessionStore& store,
        std::uint64_t groupSessionId,
        bool create,
        const state::gameplay::entity_identity::Source& source) noexcept {
    if (groupSessionId == 0 || store.catalog == nullptr) {
        return nullptr;
    }
    CompositeEntitySession* empty = nullptr;
    for (CompositeEntitySession& candidate : store.sessions) {
        if (candidate.occupied && candidate.groupSessionId == groupSessionId
            && candidate.source == source) {
            return &candidate;
        }
        if (!candidate.occupied && empty == nullptr) {
            empty = &candidate;
        }
    }
    if (!create || empty == nullptr
        || !bind_context(empty->codec, empty->registry, store.catalog, store.positionCompression)) {
        return nullptr;
    }
    empty->groupSessionId = groupSessionId;
    empty->codec.positionProfile = store.positionProfile;
    empty->codec.resolvePosition = store.resolvePosition;
    empty->codec.positionContext = store.positionContext;
    empty->codec.source = source;
    empty->codec.resolvePlan = store.resolvePlan;
    empty->codec.resolveSchemaLayout = store.resolveSchemaLayout;
    empty->codec.resolveFieldLayout = store.resolveFieldLayout;
    empty->codec.resolveAdditionalSchema = store.resolveAdditionalSchema;
    empty->codec.resolveAdditionalField = store.resolveAdditionalField;
    empty->codec.planContext = store.planContext;
    empty->source = source;
    empty->occupied = true;
    return empty;
}

} // namespace detail

/** Retained parent attachments preserve native tail-insertion order for implicit token groups. */
static bool resolve_anchor_group(const void* raw,
                                 const EntityToken& anchor,
                                 std::span<EntityToken> output,
                                 std::size_t& count) noexcept {
    count = 0;
    const auto* context = static_cast<const CompositeEntityCodecContext*>(raw);
    if (context == nullptr || context->registry == nullptr || !detail::valid_token(anchor)
        || output.empty()) {
        return false;
    }
    const auto& slots = context->registry->slots;
    if (!slots[anchor.slot].occupied || slots[anchor.slot].incarnation != anchor.incarnation) {
        return false;
    }
    std::array<EntityToken, kEntityBatchCapacity> parents{};
    std::array<std::uint64_t, kEntityBatchCapacity> siblingOrders{};
    std::size_t depth = 0;
    EntityToken next = anchor;
    for (;;) {
        if (count == output.size() || count == kEntityBatchCapacity) {
            count = 0;
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (output[index].slot == next.slot) {
                count = 0;
                return false;
            }
        }
        output[count++] = next;
        parents[depth] = next;
        siblingOrders[depth] = 0;
        for (;;) {
            const auto parent = parents[depth];
            std::size_t child = slots.size();
            std::uint64_t earliest = (std::numeric_limits<std::uint64_t>::max)();
            for (std::size_t index = 0; index < slots.size(); ++index) {
                const auto& slot = slots[index];
                if (slot.occupied && slot.anchorPresent && slot.anchor.slot == parent.slot
                    && slot.anchor.incarnation == parent.incarnation
                    && slot.anchorOrder > siblingOrders[depth] && slot.anchorOrder < earliest) {
                    earliest = slot.anchorOrder;
                    child = index;
                }
            }
            if (child != slots.size()) {
                siblingOrders[depth] = earliest;
                if (++depth == parents.size()) {
                    count = 0;
                    return false;
                }
                next = {static_cast<std::uint16_t>(child), slots[child].incarnation};
                break;
            }
            if (depth == 0) {
                return true;
            }
            --depth;
        }
    }
}

/** Builds pure callbacks over one stable codec context. */
TypePayloadCodec
make_composite_entity_payload_codec(const CompositeEntityCodecContext& context) noexcept {
    TypePayloadCodec codec{};
    codec.context = &context;
    codec.resolveType = detail::resolve_type;
    codec.read = detail::read_payload;
    codec.write = detail::write_payload;
    codec.readForCell = detail::read_cell_payload;
    codec.writeForCell = detail::write_cell_payload;
    codec.maximumBaselineBits = kMaximumTypePayloadBits;
    codec.maximumUpdateBits = kMaximumTypePayloadBits;
    codec.resolveAnchorGroup = resolve_anchor_group;
    return codec;
}

/** Validates one record against its original or earlier staged slot state. */
static bool stage_entity_record_mutation(const CompositeEntityCodecContext& context,
                                         const EntityRecord& record,
                                         const EntityBaselineSlot& current,
                                         EntityBaselineChange& output,
                                         bool resetSerial) noexcept {
    output = {};
    if (!context.ready || context.registry == nullptr
        || (context.catalog != nullptr && context.registry->catalog != context.catalog)
        || !detail::valid_token(record.token)) {
        return false;
    }
    EntityBaselineChange candidate{};
    candidate.expected = current;
    candidate.replacement = current;
    candidate.slot = record.token.slot;

    if ((record.flags & entityCreate) != 0) {
        const format::EntityTypeDefinition* definition =
            detail::entity_definition(context, record.type);
        if (definition == nullptr || (definition->flags & format::kEntityTypeStockEmittable) == 0) {
            return false;
        }
        if (!resetSerial && (current.known || current.occupied)) {
            const bool duplicate = current.occupied
                                   && current.incarnation == record.token.incarnation
                                   && current.allocationSequence == record.allocationSequence
                                   && current.type == record.type;
            if (!duplicate
                && !serial_is_newer(record.allocationSequence, current.allocationSequence)) {
                return false;
            }
        }
        std::uint32_t rsatTag = 0;
        bool placement = true;
        if (record.type == EntityType::sobject) {
            detail::MirrorView baseline{};
            if (!detail::load_mirror(record.baseline, baseline) || baseline.header.semanticTag == 0
                || detail::sobject_rsat(context, baseline.header.semanticTag) == nullptr) {
                return false;
            }
            rsatTag = baseline.header.semanticTag;
            placement = (std::to_integer<unsigned>(baseline.bytes.back()) & 1U) != 0;
        }
        if (current.occupied && current.incarnation == record.token.incarnation
            && current.allocationSequence == record.allocationSequence
            && current.type == record.type
            && (current.rsatTag != rsatTag || current.sobjectPlacement != placement)) {
            return false;
        }
        candidate.replacement.rsatTag = rsatTag;
        candidate.replacement.allocationSequence = record.allocationSequence;
        candidate.replacement.incarnation = record.token.incarnation;
        candidate.replacement.type = record.type;
        candidate.replacement.occupied = true;
        candidate.replacement.known = true;
        candidate.replacement.sobjectPlacement = placement;
        const bool retainedObject = resetSerial && current.occupied
                                    && current.incarnation == record.token.incarnation
                                    && current.type == record.type && current.rsatTag == rsatTag;
        if (!retainedObject
            && (!current.occupied || current.incarnation != record.token.incarnation
                || current.allocationSequence != record.allocationSequence)) {
            candidate.replacement.anchor = {};
            candidate.replacement.anchorPresent = false;
        }
    } else {
        if (!current.occupied || current.incarnation != record.token.incarnation) {
            return false;
        }
        const format::EntityTypeDefinition* definition =
            detail::entity_definition(context, current.type);
        if (definition == nullptr
            || ((record.flags & entityUpdate) != 0
                && (definition->flags & format::kEntityTypeUpdateSupported) == 0)) {
            return false;
        }
    }
    if ((record.flags & entityAnchor) != 0) {
        candidate.replacement.anchor = record.anchorPresent ? record.anchor : EntityToken{};
        candidate.replacement.anchorPresent = record.anchorPresent;
    }
    output = candidate;
    return true;
}

/** Every record is staged before any baseline slot changes. */
bool stage_entity_baseline_mutation(const CompositeEntityCodecContext& context,
                                    const EntityBatch& batch,
                                    EntityBaselineMutation& output) noexcept {
    output = {};
    const auto count = entity_record_count(batch);
    if (context.registry == nullptr || count == 0 || count > kEntityBatchCapacity) {
        return false;
    }
    EntityBaselineMutation candidate{};
    candidate.expectedAnchorOrder = context.registry->anchorOrder;
    candidate.replacementAnchorOrder = candidate.expectedAnchorOrder;
    candidate.expectedAllocationEpoch = context.registry->allocationEpoch;
    candidate.expectedHasAllocationEpoch = context.registry->hasAllocationEpoch;
    candidate.expectedAllocationDomain = context.registry->allocationDomain;
    candidate.replacementAllocationEpoch = candidate.expectedAllocationEpoch;
    candidate.replacementHasAllocationEpoch = candidate.expectedHasAllocationEpoch;
    candidate.replacementAllocationDomain = candidate.expectedAllocationDomain;
    if (batch.hasAllocationEpoch) {
        if (batch.allocationDomain == 0
            || (context.registry->hasAllocationEpoch
                && (batch.allocationEpoch != context.registry->allocationEpoch
                    || batch.allocationDomain != context.registry->allocationDomain))) {
            return false;
        }
        candidate.replacementAllocationEpoch = batch.allocationEpoch;
        candidate.replacementHasAllocationEpoch = true;
        candidate.replacementAllocationDomain = batch.allocationDomain;
    } else if (context.registry->hasAllocationEpoch) {
        return false;
    }
    std::size_t changes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = entity_record_at(batch, index);
        if (!detail::valid_token(record.token)) {
            return false;
        }
        std::size_t selected = 0;
        for (; selected < changes; ++selected) {
            const auto& change = selected == 0 ? static_cast<const EntityBaselineChange&>(candidate)
                                               : candidate.additionalChanges[selected - 1];
            if (change.slot == record.token.slot) {
                break;
            }
        }
        auto& change = selected == 0 ? static_cast<EntityBaselineChange&>(candidate)
                                     : candidate.additionalChanges[selected - 1];
        EntityBaselineChange next{};
        const auto& current =
            selected < changes ? change.replacement : context.registry->slots[record.token.slot];
        const bool resetSerial =
            batch.hasAllocationEpoch && current.serialDomain != batch.allocationDomain;
        if (batch.hasAllocationEpoch && current.occupied && (record.flags & entityCreate) != 0
            && record.allocationSequence == 0) {
            return false;
        }
        if (batch.hasAllocationEpoch && !record.implicitToken && !current.occupied
            && (record.flags & (entityCreate | entityUpdate | entityRemove)) == entityRemove) {
            candidate.ignoredRecordMask.set(index);
            continue;
        }
        if (batch.hasAllocationEpoch && !record.implicitToken && !current.occupied
            && (record.flags & entityCreate) != 0
            && (record.allocationSequence == 0
                || (current.known && !resetSerial
                    && !serial_is_newer(record.allocationSequence, current.allocationSequence)))) {
            candidate.ignoredRecordMask.set(index);
            continue;
        }
        if (!stage_entity_record_mutation(context, record, current, next, resetSerial)) {
            return false;
        }
        if ((record.flags & entityCreate) != 0 && batch.hasAllocationEpoch) {
            next.replacement.serialDomain = batch.allocationDomain;
        }
        if ((record.flags & entityCreate) != 0 && batch.hasAllocationEpoch
            && (!current.occupied || !current.known
                || current.incarnation != record.token.incarnation
                || current.allocationSequence != record.allocationSequence
                || current.type != record.type)) {
            next.replacement.hasAllocationEpoch = true;
            next.replacement.allocationEpoch = batch.allocationEpoch;
            next.replacement.allocationDomain = batch.allocationDomain;
        }
        const bool identicalCreateAnchor =
            (record.flags & entityCreate) != 0 && current.occupied
            && current.incarnation == record.token.incarnation
            && current.allocationSequence == record.allocationSequence
            && current.anchorPresent == next.replacement.anchorPresent
            && current.anchor.slot == next.replacement.anchor.slot
            && current.anchor.incarnation == next.replacement.anchor.incarnation;
        if (next.replacement.occupied && !identicalCreateAnchor
            && (((record.flags & entityAnchor) != 0)
                || ((record.flags & entityCreate) != 0 && !current.occupied))) {
            if (candidate.replacementAnchorOrder == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            next.replacement.anchorOrder =
                next.replacement.anchorPresent ? ++candidate.replacementAnchorOrder : 0;
        }
        if (selected < changes) {
            next.expected = change.expected;
        } else {
            ++changes;
        }
        change = next;
    }
    const auto change_at = [&](std::size_t index) -> EntityBaselineChange& {
        return index == 0 ? static_cast<EntityBaselineChange&>(candidate)
                          : candidate.additionalChanges[index - 1];
    };
    const auto current_slot = [&](std::size_t slot) -> const EntityBaselineSlot& {
        for (std::size_t index = 0; index < changes; ++index) {
            if (change_at(index).slot == slot) {
                return change_at(index).replacement;
            }
        }
        return context.registry->slots[slot];
    };
    // The native anchor pass (0x141713250) runs before its remove pass: an entity that receives a
    // record detaches to root every descendant that received none in the same batch. A parent sent
    // as a direct element therefore loses its children, and they outlive its removal as roots.
    std::bitset<kMaximumEntitySlot + 1U> recorded{};
    for (std::size_t index = 0; index < count; ++index) {
        if (!candidate.ignoredRecordMask.test(index)) {
            recorded.set(entity_record_at(batch, index).token.slot);
        }
    }
    const auto& retained = context.registry->slots;
    for (std::size_t slot = 0; slot < retained.size(); ++slot) {
        if (recorded.test(slot) || !retained[slot].occupied || !retained[slot].anchorPresent) {
            continue;
        }
        // The native walk detaches a stale node and does not descend into it, so a stale node's own
        // subtree leaves with it: only the direct parent's record matters.
        const EntityToken parent = retained[slot].anchor;
        bool stale = false;
        if (recorded.test(parent.slot)) {
            const auto& staged = current_slot(parent.slot);
            stale = staged.occupied && staged.incarnation == parent.incarnation;
        }
        if (!stale) {
            continue;
        }
        if (changes == kEntityBatchCapacity) {
            return false;
        }
        auto& change = change_at(changes++);
        change.slot = static_cast<std::uint16_t>(slot);
        change.expected = retained[slot];
        change.replacement = change.expected;
        change.replacement.anchor = {};
        change.replacement.anchorPresent = false;
        change.replacement.anchorOrder = 0;
        ++candidate.detachedCount;
    }
    std::array<EntityToken, kEntityBatchCapacity> terminals{};
    std::size_t terminalCount = 0;
    const auto append_terminal = [&](EntityToken token) {
        for (std::size_t index = 0; index < terminalCount; ++index) {
            if (terminals[index].slot == token.slot
                && terminals[index].incarnation == token.incarnation) {
                return true;
            }
        }
        if (terminalCount == terminals.size()) {
            return false;
        }
        terminals[terminalCount++] = token;
        return true;
    };
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = entity_record_at(batch, index);
        if (!candidate.ignoredRecordMask.test(index) && (record.flags & entityRemove) != 0
            && !append_terminal(record.token)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < terminalCount; ++index) {
        const auto token = terminals[index];
        const auto& before = current_slot(token.slot);
        if (!before.occupied || before.incarnation != token.incarnation) {
            continue;
        }
        for (std::size_t slot = 0; slot < context.registry->slots.size(); ++slot) {
            const auto& child = current_slot(slot);
            if (child.occupied && child.anchorPresent && child.anchor.slot == token.slot
                && child.anchor.incarnation == token.incarnation
                && !append_terminal({static_cast<std::uint16_t>(slot), child.incarnation})) {
                return false;
            }
        }
        std::size_t selected = 0;
        for (; selected < changes && change_at(selected).slot != token.slot; ++selected) {}
        if (selected == changes) {
            if (changes == kEntityBatchCapacity) {
                return false;
            }
            auto& change = change_at(changes++);
            change.slot = token.slot;
            change.expected = context.registry->slots[token.slot];
            change.replacement = change.expected;
        }
        change_at(selected).replacement.occupied = false;
        change_at(selected).replacement.known = true;
    }
    candidate.hasChanges = changes != 0;
    candidate.additionalChangeCount = static_cast<std::uint16_t>(changes == 0 ? 0 : changes - 1);
    candidate.valid = true;
    output = candidate;
    return true;
}

/** Applies a staged mutation only when its source slot has not changed. */
bool commit_accepted_entity_batch(EntityBaselineRegistry& registry,
                                  const EntityBaselineMutation& mutation) noexcept {
    if (!mutation.valid || mutation.additionalChangeCount >= kEntityBatchCapacity
        || registry.anchorOrder != mutation.expectedAnchorOrder
        || registry.allocationEpoch != mutation.expectedAllocationEpoch
        || registry.hasAllocationEpoch != mutation.expectedHasAllocationEpoch
        || registry.allocationDomain != mutation.expectedAllocationDomain) {
        return false;
    }
    for (std::size_t index = 0; mutation.hasChanges && index <= mutation.additionalChangeCount;
         ++index) {
        const auto& change = index == 0 ? static_cast<const EntityBaselineChange&>(mutation)
                                        : mutation.additionalChanges[index - 1];
        if (change.slot > kMaximumEntitySlot
            || !same_slot(registry.slots[change.slot], change.expected)) {
            return false;
        }
    }
    for (std::size_t index = 0; mutation.hasChanges && index <= mutation.additionalChangeCount;
         ++index) {
        const auto& change = index == 0 ? static_cast<const EntityBaselineChange&>(mutation)
                                        : mutation.additionalChanges[index - 1];
        registry.slots[change.slot] = change.replacement;
    }
    registry.anchorOrder = mutation.replacementAnchorOrder;
    registry.allocationEpoch = mutation.replacementAllocationEpoch;
    registry.hasAllocationEpoch = mutation.replacementHasAllocationEpoch;
    registry.allocationDomain = mutation.replacementAllocationDomain;
    return true;
}

/** Direct construction avoids a multi-megabyte temporary on the caller stack. */
void reset_composite_entity_sessions(CompositeEntitySessionStore& store) noexcept {
    std::destroy_at(&store);
    std::construct_at(&store);
}

/** Pins one catalog for a new bounded session store. */
bool initialize_composite_entity_sessions(CompositeEntitySessionStore& store,
                                          SobjectPositionCompression positionCompression) noexcept {
    state::activity_sdk::Snapshot catalog = state::activity_sdk::snapshot();
    const std::unique_ptr<CompositeEntitySession> probe(new (std::nothrow)
                                                            CompositeEntitySession{});
    if (!probe
        || !detail::bind_context(probe->codec, probe->registry, catalog, positionCompression)) {
        return false;
    }
    reset_composite_entity_sessions(store);
    store.catalog = std::move(catalog);
    store.positionCompression = positionCompression;
    return true;
}

/** Decodes one batch against the named session's committed baselines. */
bool read_composite_entity_batch(CompositeEntitySessionStore& store,
                                 std::uint64_t groupSessionId,
                                 bits::Reader& reader,
                                 EntityBatch& output) noexcept {
    CompositeEntitySession* selected = detail::session(store, groupSessionId, true);
    if (selected == nullptr) {
        return false;
    }
    const TypePayloadCodec codec = make_composite_entity_payload_codec(selected->codec);
    return read_entity_batch(reader, codec, output);
}

/** Stages and commits one peer-accepted batch in its named session. */
bool accept_composite_entity_batch(const void* context,
                                   std::uint64_t groupSessionId,
                                   const EntityBatch& batch) noexcept {
    auto* const store = const_cast<CompositeEntitySessionStore*>(
        static_cast<const CompositeEntitySessionStore*>(context));
    if (store == nullptr) {
        return false;
    }
    CompositeEntitySession* selected = detail::session(*store, groupSessionId, false);
    EntityBaselineMutation mutation{};
    return selected != nullptr && stage_entity_baseline_mutation(selected->codec, batch, mutation)
           && commit_accepted_entity_batch(selected->registry, mutation);
}

/** Releases every baseline owned by one ended session. */
void reset_composite_entity_session(CompositeEntitySessionStore& store,
                                    std::uint64_t groupSessionId) noexcept {
    for (auto& selected : store.sessions) {
        if (selected.occupied && selected.groupSessionId == groupSessionId) {
            std::destroy_at(&selected);
            std::construct_at(&selected);
        }
    }
}

/** Removes baselines for only the exact retired source. */
void reset_scoped_entity_session(CompositeEntitySessionStore& store,
                                 const state::gameplay::entity_identity::Source& source) noexcept {
    for (auto& selected : store.sessions) {
        if (selected.occupied && selected.source == source) {
            std::destroy_at(&selected);
            std::construct_at(&selected);
        }
    }
}

/** Only matching allocations lose occupancy; their serial and ordering evidence remains. */
std::size_t retire_scoped_entity_baselines(
    CompositeEntitySessionStore& store,
    const state::gameplay::entity_identity::Source& source,
    std::span<const state::gameplay::entity_identity::RetiredLifetime> lifetimes) noexcept {
    auto* selected = detail::session(store, source.groupSessionId, false, source);
    if (selected == nullptr) {
        return 0;
    }
    std::size_t retired = 0;
    for (const auto& lifetime : lifetimes) {
        if (!detail::valid_token({lifetime.token.slot, lifetime.token.incarnation})) {
            continue;
        }
        auto& slot = selected->registry.slots[lifetime.token.slot];
        if (!slot.known || !slot.occupied || slot.incarnation != lifetime.token.incarnation
            || slot.allocationSequence != lifetime.allocationSequence
            || slot.allocationEpoch != lifetime.allocationEpoch
            || slot.allocationDomain != lifetime.allocationDomain) {
            continue;
        }
        slot.occupied = false;
        ++retired;
    }
    return retired;
}

/** An absent source has no serial domain to reset; existing identity evidence remains intact. */
bool advance_scoped_entity_epoch(CompositeEntitySessionStore& store,
                                 const state::gameplay::entity_identity::Source& source,
                                 std::uint8_t expected,
                                 std::uint8_t next,
                                 std::uint64_t nextDomain) noexcept {
    if (nextDomain == 0 || next != static_cast<std::uint8_t>(expected + 1U)) {
        return false;
    }
    auto* selected = detail::session(store, source.groupSessionId, false, source);
    if (selected == nullptr) {
        return true;
    }
    auto& registry = selected->registry;
    if (registry.hasAllocationEpoch
        && (registry.allocationEpoch != expected || nextDomain != registry.allocationDomain + 1U)) {
        return false;
    }
    registry.hasAllocationEpoch = true;
    registry.allocationEpoch = next;
    registry.allocationDomain = nextDomain;
    return true;
}

/** Decodes only against this admitted source's committed baselines. */
bool read_scoped_entity_batch(CompositeEntitySessionStore& store,
                              const state::gameplay::entity_identity::Source& source,
                              bits::Reader& reader,
                              EntityBatch& output) noexcept {
    auto* selected = detail::session(store, source.groupSessionId, true, source);
    if (selected == nullptr) {
        return false;
    }
    return read_entity_batch(reader, make_composite_entity_payload_codec(selected->codec), output);
}

/**
 * Expanded peer ordinals keep sparse entity updates ordered across wire-sequence wraps.
 * @param store Caller-owned baseline store, unchanged by preparation.
 * @param source Exact admitted source whose decoder already selected a session.
 * @param batch Complete decoded record.
 * @param packetSequence Original ten-bit wire sequence.
 * @param hasPacketSequence Whether the packet carries an ordered sequence.
 * @param packetOrdinal Sequence unwrapped across every accepted packet on the peer.
 * @param output Receives a source-bound mutation, cleared on failure.
 * @return True when lifetime and packet ordering permit a later commit.
 */
bool prepare_scoped_entity_batch(CompositeEntitySessionStore& store,
                                 const state::gameplay::entity_identity::Source& source,
                                 const EntityBatch& batch,
                                 std::uint16_t packetSequence,
                                 bool hasPacketSequence,
                                 std::uint64_t packetOrdinal,
                                 EntityBaselineMutation& output) noexcept {
    output = {};
    auto* selected = detail::session(store, source.groupSessionId, false, source);
    if (selected == nullptr || !batch.recordPresent || !detail::valid_token(batch.record.token)
        || (hasPacketSequence
            && (packetSequence >= state::gameplay::entity_identity::kPacketModulus
                || packetOrdinal == 0))) {
        return false;
    }
    EntityBaselineMutation candidate{};
    if (!stage_entity_baseline_mutation(selected->codec, batch, candidate)) {
        return false;
    }
    for (std::size_t index = 0; candidate.hasChanges && index <= candidate.additionalChangeCount;
         ++index) {
        auto& change = index == 0 ? static_cast<EntityBaselineChange&>(candidate)
                                  : candidate.additionalChanges[index - 1];
        if (change.expected.hasPacketSequence
            && (!hasPacketSequence || packetOrdinal <= change.expected.packetOrdinal)) {
            return false;
        }
        change.replacement.hasPacketSequence = hasPacketSequence;
        change.replacement.packetSequence = hasPacketSequence ? packetSequence : 0;
        change.replacement.packetOrdinal = hasPacketSequence ? packetOrdinal : 0;
    }
    candidate.source = source;
    candidate.scoped = true;
    output = candidate;
    return true;
}

/**
 * Commit changes one existing slot only when its prepared source and prior state still match.
 * @param store Caller-owned baseline store.
 * @param source Exact source used by preparation.
 * @param mutation Prepared source-bound replacement.
 * @return True when the compare-and-commit succeeds without allocation.
 */
bool commit_scoped_entity_batch(CompositeEntitySessionStore& store,
                                const state::gameplay::entity_identity::Source& source,
                                const EntityBaselineMutation& mutation) noexcept {
    if (!mutation.scoped || mutation.source != source) {
        return false;
    }
    auto* selected = detail::session(store, source.groupSessionId, false, source);
    return selected != nullptr && commit_accepted_entity_batch(selected->registry, mutation);
}

/** Commits the last fallible channel-2 operation for this source. */
bool accept_scoped_entity_batch(CompositeEntitySessionStore& store,
                                const state::gameplay::entity_identity::Source& source,
                                const EntityBatch& batch) noexcept {
    auto* selected = detail::session(store, source.groupSessionId, false, source);
    EntityBaselineMutation mutation{};
    return selected != nullptr && stage_entity_baseline_mutation(selected->codec, batch, mutation)
           && commit_accepted_entity_batch(selected->registry, mutation);
}

} // namespace sunrise::middleware::gameplay::external
