#include "scenario_walk.h"

#include <array>

namespace sunrise::middleware::content::packages::tables {
namespace {

/** The three registry descriptors, walked in this order to match the reference walk. */
constexpr std::array<std::size_t, 3> kRegistryDescriptors = {
    kRegistryFirstDescriptor,
    kRegistrySecondDescriptor,
    kRegistryThirdDescriptor,
};

/** One state's resolved entry and registry, with the bytes still live. */
struct Resolved {
    std::span<const std::byte> registry;
    std::uint32_t registryTag{};
    std::uint32_t sliceSetIndex{};
};

/**
 * Reads a state's slice-set entry and then its object registry.
 * @param entryTag Tag the state names.
 * @param output Receives the registry bytes and the entry fields.
 * @return True when both blobs read and the entry parses.
 */
[[nodiscard]] bool
resolve_state(TagReader reader, void* context, std::uint32_t entryTag, Resolved& output) noexcept {
    output = {};
    std::span<const std::byte> entryBlob;
    if (!reader(context, ReadSlot::entry, entryTag, entryBlob)) {
        return false;
    }
    SliceEntry entry{};
    if (!slice_entry(entryBlob, entry)) {
        return false;
    }
    std::span<const std::byte> registryBlob;
    if (!reader(context, ReadSlot::registry, entry.registryTag, registryBlob)) {
        return false;
    }
    output.registry = registryBlob;
    output.registryTag = entry.registryTag;
    output.sliceSetIndex = entry.index * kSliceSetIndexFactor;
    return true;
}

/** Everything one placement needs that does not come from the object itself. */
struct StateContext {
    std::uint32_t bubbleHash{};
    std::uint64_t bubbleIndex{};
    std::uint64_t stateIndex{};
    const SliceState* state{};
    const Resolved* resolved{};
    bool bubblePublic{};
};

/**
 * Emits one placement per object in one registry array.
 * @param state Fixed fields for the owning state.
 * @param descriptor Which registry array is being walked.
 * @param visitor Placement consumer.
 * @param result Receives the placement count.
 * @return True when the visitor accepts every placement.
 */
[[nodiscard]] bool emit_objects(const StateContext& state,
                                std::size_t descriptor,
                                TagReader reader,
                                void* readerContext,
                                PlacementVisitor visitor,
                                void* visitorContext,
                                WalkResult& result) noexcept {
    Array objects{};
    if (!registry_objects(state.resolved->registry, descriptor, objects)) {
        // A registry declaring only some of its three arrays is ordinary.
        return true;
    }
    for (std::uint64_t index = 0; index < objects.count; ++index) {
        std::uint32_t objectTag = 0;
        if (!registry_object_at(state.resolved->registry, objects, index, objectTag)) {
            return false;
        }
        std::span<const std::byte> objectBlob;
        if (!reader(readerContext, ReadSlot::object, objectTag, objectBlob)) {
            ++result.unresolved;
            continue;
        }
        Placement placement{};
        placement.bubbleHash = state.bubbleHash;
        placement.bubbleIndex = state.bubbleIndex;
        placement.stateIndex = state.stateIndex;
        placement.stateHash = state.state->stateHash;
        placement.stateEnabled = state.state->enabled;
        placement.bubblePublic = state.bubblePublic;
        placement.entryTag = state.state->entryTag;
        placement.sliceSetIndex = state.resolved->sliceSetIndex;
        placement.registryTag = state.resolved->registryTag;
        placement.registryDescriptor = descriptor;
        placement.objectIndex = index;
        placement.objectTag = objectTag;
        if (!object_key(objectBlob, placement.objectKey)) {
            return false;
        }
        Array slots{};
        if (object_slots(objectBlob, slots)) {
            placement.slotCount = slots.count;
        }
        placement.objectBytes = objectBlob;
        ++result.placements;
        if (!visitor(visitorContext, placement)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Walks a scenario and emits one placement per object of every resolvable slice-set state. */
bool walk_scenario(std::span<const std::byte> blob,
                   TagReader reader,
                   void* readerContext,
                   PlacementVisitor visitor,
                   void* visitorContext,
                   WalkResult& output) noexcept {
    output = {};
    if (reader == nullptr || visitor == nullptr) {
        return false;
    }
    Array bubbles{};
    if (!scenario_bubbles(blob, bubbles)) {
        return false;
    }
    for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count; ++bubbleIndex) {
        Bubble bubble{};
        if (!bubble_at(blob, bubbles, bubbleIndex, bubble)) {
            return false;
        }
        ++output.bubbles;
        bool bubblePublic = false;
        for (std::uint64_t stateIndex = 0; stateIndex < bubble.stateCount; ++stateIndex) {
            SliceState state{};
            if (!slice_state_at(blob, bubble, stateIndex, state)) {
                return false;
            }
            ++output.states;
            // Only the first state carries the flag the client prints the bubble prefix from.
            if (stateIndex == 0) {
                bubblePublic = state.isPublic;
            }
            Resolved resolved{};
            if (!resolve_state(reader, readerContext, state.entryTag, resolved)) {
                ++output.unresolved;
                continue;
            }
            ++output.entries;
            ++output.registries;
            const StateContext context{
                bubble.nameHash, bubbleIndex, stateIndex, &state, &resolved, bubblePublic};
            for (const std::size_t descriptor : kRegistryDescriptors) {
                if (!emit_objects(context,
                                  descriptor,
                                  reader,
                                  readerContext,
                                  visitor,
                                  visitorContext,
                                  output)) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
