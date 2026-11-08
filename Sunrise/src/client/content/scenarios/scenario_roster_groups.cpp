#include <array>
#include <atomic>
#include <cstdio>
#include <span>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/**
 * Records one descriptor as a slot of the object being resolved.
 * @param context Roster storage.
 * @param descriptor Descriptor read from a placed-object blob.
 * @return Always true, because a descriptor this pass cannot use is ordinary.
 */
bool collect_slot(void* context, const tables::SlotDescriptor& descriptor) noexcept {
    record_slot(*static_cast<RosterStorage*>(context), descriptor);
    return true;
}

/** Package state held across one injected descriptor-chain read. */
struct ChainReadContext {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    RosterStorage* storage{};
};

/** Reads one descriptor-chain tag into the roster pass's borrowed blob storage. */
[[nodiscard]] bool read_chain_tag(void* context,
                                  std::uint32_t tag,
                                  std::span<const std::byte>& blob,
                                  std::uint32_t& classId) noexcept {
    auto& chain = *static_cast<ChainReadContext*>(context);
    ++chain.storage->reads;
    if (!reader::read_tag(*chain.source, *chain.scratch, tag, chain.storage->chain, classId)) {
        ++chain.storage->exits.readFailures;
        blob = {};
        return false;
    }
    blob = std::span<const std::byte>{chain.storage->chain};
    return true;
}

/**
 * Follows every branch of one placed handle and records what its descriptor blobs declare.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param handle Tag from a placed object's per-bubble sub-block.
 * @param registryKey Registry key the descriptors must name.
 * @return True only when the bounded chain and every config's descriptor read finished.
 */
[[nodiscard]] bool follow_handle(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 RosterStorage& storage,
                                 std::uint32_t handle,
                                 std::uint32_t registryKey) noexcept {
    ChainReadContext context{&source, &scratch, &storage};
    return tables::walk_slot_descriptor_chain(
        handle, registryKey, &read_chain_tag, &context, &collect_slot, &storage);
}

/**
 * Collects every descriptor one group object declares, over all of its per-bubble sub-blocks.
 * Every leaf is followed: one leaf is one slot, so stopping early would drop slots rather than
 * merely leave a slot type unresolved.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage receiving the descriptors.
 * @param objectBlob Whole placed-object bytes.
 * @param registryKey Registry key the descriptors must name.
 * @return True only when every declared placed handle was read and walked completely.
 */
[[nodiscard]] bool collect_descriptors(const reader::Source& source,
                                       reader::Scratch& scratch,
                                       RosterStorage& storage,
                                       std::span<const std::byte> objectBlob,
                                       std::uint32_t registryKey) noexcept {
    tables::Array bubbles{};
    if (!tables::object_bubbles(objectBlob, bubbles)) {
        return false;
    }
    for (std::uint64_t index = 0; index < bubbles.count; ++index) {
        tables::ObjectBubble bubble{};
        if (!tables::object_bubble_at(objectBlob, bubbles, index, bubble)) {
            ++storage.exits.bubbleAborts;
            return false;
        }
        for (std::uint64_t slot = 0; slot < bubble.handleCount; ++slot) {
            std::uint32_t handle = 0;
            if (!tables::object_placed_handle_at(objectBlob, bubble, slot, handle)) {
                ++storage.exits.handleAborts;
                return false;
            }
            ++storage.exits.handles;
            if (!follow_handle(source, scratch, storage, handle, registryKey)) {
                return false;
            }
        }
    }
    return true;
}

/** Unresolved group objects reported per walk. Bounds the sink on a tree that drops many. */
constexpr std::size_t kMaxUnresolvedReports = 128;
/** Size of one line, set by its tag, key and the per-exit counts that follow them. */
constexpr std::size_t kUnresolvedLineCapacity = 256;

/** Lines already spent, so a long walk cannot flood the sink. */
std::atomic_size_t g_unresolvedReports{0};

/**
 * Names one group object the descriptor walk could not fill.
 * The declared-to-found gap says whether the chain stopped early or the classification refused it.
 * @param objectTag Tag of the object being resolved.
 * @param registryKey Registry key the object declares.
 * @param declaredSlotCount Slots the object's own slot array declares.
 * @param storage Working storage holding what the walk recovered.
 */
void report_unresolved(std::uint32_t objectTag,
                       std::uint32_t registryKey,
                       std::uint64_t declaredSlotCount,
                       const RosterStorage& storage) noexcept {
    // One atomic claim per line, so a concurrent walk cannot reuse a budget slot.
    if (g_unresolvedReports.fetch_add(1, std::memory_order_relaxed) >= kMaxUnresolvedReports) {
        return;
    }
    std::array<char, kUnresolvedLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=roster result=unresolved tag=0x%08X "
                                      "key=0x%08X declared=%llu found=%zu overflow=%u "
                                      "handles=%zu blobs=%zu bubble_abort=%zu handle_abort=%zu "
                                      "read_fail=%zu chain_end=%zu depth=%zu",
                                      objectTag,
                                      registryKey,
                                      static_cast<unsigned long long>(declaredSlotCount),
                                      storage.slotCount,
                                      storage.slotsOverflowed ? 1U : 0U,
                                      storage.exits.handles,
                                      storage.exits.blobs,
                                      storage.exits.bubbleAborts,
                                      storage.exits.handleAborts,
                                      storage.exits.readFailures,
                                      storage.exits.chainEnds,
                                      storage.exits.depthExhausted);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Objects named per run by the placement trace.
 * The installed tree holds 5,991 placed objects and each is traced once, so this shows every one.
 */
constexpr std::size_t kMaxPlacementReports = 8192;
/** Slot types listed per line. No installed object declares more than this many. */
constexpr std::size_t kTracedSlotTypes = 24;
/** Size of one line: the fixed fields plus up to `kTracedSlotTypes` short decimal numbers. */
constexpr std::size_t kPlacementLineCapacity = 256;

/** Lines already spent, so a full content walk cannot flood the sink. */
std::atomic_size_t g_placementReports{0};

/**
 * Names one placed object and every slot type it declares, before any filter has judged it.
 * An object `carries_roster_slot` refuses leaves no other trace, so the two empty cases read alike.
 * @param destinationTag Destination whose scenario named this object.
 * @param sliceSetIndex Slice set whose registry named this object.
 * @param objectTag Tag of the placed object.
 * @param object Whole placed-object bytes.
 * @param admitted Whether `carries_roster_slot` accepted it.
 */
void report_placement(std::uint32_t destinationTag,
                      std::uint32_t sliceSetIndex,
                      std::uint32_t objectTag,
                      std::span<const std::byte> object,
                      bool admitted) noexcept {
    if (!core::log::accepts(core::log::Channel::state, core::log::Level::debug)) {
        return;
    }
    // One atomic claim per line, so a concurrent walk cannot reuse a budget slot.
    if (g_placementReports.fetch_add(1, std::memory_order_relaxed) >= kMaxPlacementReports) {
        return;
    }
    std::uint32_t key = 0;
    (void)tables::object_key(object, key);
    tables::Array slots{};
    const bool hasSlots = tables::object_slots(object, slots);
    std::array<char, kPlacementLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=build_data stage=placement dest=0x%08X slice=%u bubble=%u "
                                "tag=0x%08X key=0x%08X admitted=%u slots=%llu types=",
                                destinationTag,
                                sliceSetIndex,
                                sliceSetIndex / tables::kSliceSetIndexFactor,
                                objectTag,
                                key,
                                admitted ? 1U : 0U,
                                hasSlots ? static_cast<unsigned long long>(slots.count) : 0ULL);
    if (written <= 0) {
        return;
    }
    auto used = static_cast<std::size_t>(written);
    const std::uint64_t listed =
        hasSlots && slots.count < kTracedSlotTypes ? slots.count : kTracedSlotTypes;
    for (std::uint64_t index = 0; hasSlots && index < listed && used < line.size(); ++index) {
        tables::Slot slot{};
        if (!tables::object_slot_at(object, slots, index, slot)) {
            break;
        }
        written = std::snprintf(
            line.data() + used, line.size() - used, index == 0 ? "%u" : ",%u", slot.type);
        if (written <= 0) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }
    core::log::write(core::log::Channel::state, core::log::Level::debug, {line.data(), used});
}

/** @param storage Working storage. @param tag Object tag. @return Its memo slot, or capacity. */
[[nodiscard]] std::size_t memo_slot(const RosterStorage& storage, std::uint32_t tag) noexcept {
    std::size_t probe = tag % kObjectMemoCapacity;
    for (std::size_t step = 0; step < kObjectMemoCapacity; ++step) {
        if (storage.memo[probe].tag == 0 || storage.memo[probe].tag == tag) {
            return probe;
        }
        probe = (probe + 1) % kObjectMemoCapacity;
    }
    return kObjectMemoCapacity;
}

} // namespace

/**
 * Finds the roster group of one placed object, reading it only the first time it is seen.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param objectTag Tag from an object registry.
 * @param group Receives the roster group index, or the not-a-group sentinel.
 * @return True when the object was read or was already known.
 */
bool resolve_object(const reader::Source& source,
                    reader::Scratch& scratch,
                    RosterStorage& storage,
                    std::uint32_t objectTag,
                    std::uint32_t sliceSetIndex,
                    std::uint16_t& group) noexcept {
    group = kNotARosterGroup;
    const std::size_t slot = memo_slot(storage, objectTag);
    if (slot == kObjectMemoCapacity) {
        return false;
    }
    if (storage.memo[slot].tag == objectTag) {
        // The memo spans the whole pass, so an object first seen under another destination is
        // answered from here and never re-traced. A destination's own trace is therefore its
        // first sighting of each object, not every registry that names it.
        group = storage.memo[slot].group;
        return true;
    }
    storage.memo[slot].tag = objectTag;
    storage.memo[slot].group = kNotARosterGroup;
    ++storage.reads;
    if (!reader::read_tag(source, scratch, objectTag, storage.object)) {
        return true;
    }

    report_placement(storage.destinationTag,
                     sliceSetIndex,
                     objectTag,
                     storage.object,
                     tables::carries_roster_slot(storage.object));

    layouts::RosterGroup candidate{};
    tables::Array declared{};
    if (!tables::object_key(storage.object, candidate.registryKey) || candidate.registryKey == 0
        || !tables::carries_roster_slot(storage.object)
        || !tables::object_slots(storage.object, declared) || declared.count == 0
        || declared.count > layouts::kRosterSlotCapacity) {
        return true;
    }
    storage.slotCount = 0;
    storage.slotsOverflowed = false;
    storage.exits = {};
    if (!collect_descriptors(source, scratch, storage, storage.object, candidate.registryKey)
        || !fill_slots(storage, declared.count, candidate)) {
        report_unresolved(objectTag, candidate.registryKey, declared.count, storage);
        // A completed walk may prove that some declared slots have no descriptor. A failed walk
        // cannot distinguish that absence from unread content, so it refuses the whole group.
        ++storage.unresolvedGroups;
        return true;
    }
    candidate.objectTag = objectTag;
    // One key may carry different layouts in different activities, so only exact layouts reuse.
    for (std::size_t index = 0; index < storage.groupCount; ++index) {
        if (same_group_layout(storage.groups[index], candidate)) {
            storage.memo[slot].group = static_cast<std::uint16_t>(index);
            group = storage.memo[slot].group;
            return true;
        }
    }
    if (storage.groupCount == layouts::kRosterGroupCapacity) {
        return false;
    }
    storage.groups[storage.groupCount] = candidate;
    storage.memo[slot].group = static_cast<std::uint16_t>(storage.groupCount);
    group = storage.memo[slot].group;
    ++storage.groupCount;
    return true;
}

} // namespace sunrise::client::content::scenarios
