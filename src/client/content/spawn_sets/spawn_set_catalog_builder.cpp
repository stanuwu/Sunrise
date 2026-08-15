#include "spawn_set_catalog_builder.h"

#include <algorithm>
#include <limits>
#include <string_view>

#include "../../../middleware/content/packages/tables/component_container_reader.h"
#include "../../../middleware/content/packages/tables/spawn_reader.h"
#include "../../../state/build_data/spawn_sets/spawn_set_catalog.h"

namespace sunrise::client::content::spawn_sets {
namespace {

namespace tables = middleware::content::packages::tables;

static_assert((kAccumulatorCapacity & (kAccumulatorCapacity - 1U)) == 0);

/** @param value Character. @return True for an ASCII hex digit. */
[[nodiscard]] bool hexadecimal(wchar_t value) noexcept {
    return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f')
           || (value >= L'A' && value <= L'F');
}

/** @param value Character. @return Its lowercase ASCII form, or zero when it has none. */
[[nodiscard]] char lowercase_ascii(wchar_t value) noexcept {
    if (value >= L'A' && value <= L'Z') {
        return static_cast<char>(value - L'A' + L'a');
    }
    if ((value >= L'a' && value <= L'z') || (value >= L'0' && value <= L'9') || value == L'_') {
        return static_cast<char>(value);
    }
    return '\0';
}

/** @param entry Scan row. @return Its bounded normalized stem. */
[[nodiscard]] std::string_view stem_of(const Entry& entry) noexcept {
    return {entry.stem.data(), entry.stemLength};
}

/** @param stem State row. @return Its bounded normalized stem. */
[[nodiscard]] std::string_view stem_of(const spawn_state::Stem& stem) noexcept {
    return {stem.name.data(), stem.nameLength};
}

/** @return Canonical stem then tag order. */
[[nodiscard]] bool entry_less(const Entry& left, const Entry& right) noexcept {
    if (stem_of(left) != stem_of(right)) {
        return stem_of(left) < stem_of(right);
    }
    return left.tag < right.tag;
}

/** @return Stem then hash order. */
[[nodiscard]] bool hash_less(const spawn_state::NameHash& left,
                             const spawn_state::NameHash& right) noexcept {
    return left.stemIndex != right.stemIndex ? left.stemIndex < right.stemIndex
                                             : left.value < right.value;
}

/** @param stem Stem index. @param value Name hash. @return First probe slot. */
[[nodiscard]] std::size_t hash_slot(std::uint16_t stem, std::uint32_t value) noexcept {
    /** Fixed odd mixer keeps the power-of-two accumulator probe deterministic. */
    constexpr std::uint64_t kMixer = 0x9E3779B185EBCA87ULL;
    const std::uint64_t key = (static_cast<std::uint64_t>(stem) << 32U) | value;
    return static_cast<std::size_t>((key * kMixer) & (kAccumulatorCapacity - 1U));
}

/**
 * Records which package one point's set tag came from.
 * A map package loads with the geometry. An activity package loads only for the destinations whose
 * own slice-set entries name it, so those ids are kept.
 * @param row Accumulator slot.
 * @param entry Set-tag row the point came from.
 */
void record_package(HashSlot& row, const Entry& entry) noexcept {
    if (entry.activityPackage == 0) {
        row.inMapPackage = 1;
        return;
    }
    for (std::size_t index = 0; index < row.activityPackageCount; ++index) {
        if (row.activityPackages[index] == entry.packageId) {
            return;
        }
    }
    if (row.activityPackageCount >= row.activityPackages.size()) {
        row.activityPackageOverflow = 1;
        return;
    }
    row.activityPackages[row.activityPackageCount++] = entry.packageId;
}

/**
 * Adds one point to the fixed hash accumulator.
 * The mask and the unbound flag belong to the set tag the point came from, and a hash declared by
 * several tags takes the union of theirs.
 * @param storage Pass storage.
 * @param stemIndex Owning map-package stem row.
 * @param value Spawn-set name hash.
 * @param entryIndex Set-tag row the point came from.
 * @return True when the point count stays in range.
 */
[[nodiscard]] bool record_point(Storage& storage,
                                std::uint16_t stemIndex,
                                std::uint32_t value,
                                std::size_t entryIndex) noexcept {
    const Entry& entry = storage.entries[entryIndex];
    const BubbleMask& mask = storage.tagMasks[entryIndex];
    const std::uint8_t unbound = storage.tagBound[entryIndex] == 0 ? 1U : 0U;
    std::size_t slot = hash_slot(stemIndex, value);
    for (std::size_t probe = 0; probe < storage.accumulator.size(); ++probe) {
        HashSlot& row = storage.accumulator[slot];
        if (!row.occupied) {
            if (storage.accumulatedCount >= spawn_state::kNameHashCapacity) {
                return false;
            }
            row = {value, 1, stemIndex, true, mask, unbound};
            record_package(row, entry);
            ++storage.accumulatedCount;
            return true;
        }
        if (row.stemIndex == stemIndex && row.value == value) {
            if (row.pointCount == (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            ++row.pointCount;
            for (std::size_t byte = 0; byte < row.mask.bytes.size(); ++byte) {
                row.mask.bytes[byte] |= mask.bytes[byte];
            }
            row.unbound |= unbound;
            record_package(row, entry);
            return true;
        }
        slot = (slot + 1U) & (storage.accumulator.size() - 1U);
    }
    return false;
}

/**
 * Finds the set-tag row that carries one wrapped tag.
 * @param storage Pass storage holding the tag order.
 * @param tag Wrapped resource tag.
 * @param entryIndex Receives the set-tag row.
 * @return True when the tag is one of the swept spawn sets.
 */
[[nodiscard]] bool
find_entry(const Storage& storage, std::uint32_t tag, std::size_t& entryIndex) noexcept {
    entryIndex = 0;
    std::size_t low = 0;
    std::size_t high = storage.entryCount;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const std::uint32_t candidate = storage.entries[storage.tagOrder[middle]].tag;
        if (candidate == tag) {
            entryIndex = storage.tagOrder[middle];
            return true;
        }
        if (candidate < tag) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

/**
 * Orders the collected spawn components by tag, so a member lookup is a search rather than a scan.
 * Every container member is looked up, and the containers hold hundreds of thousands of them.
 * @param storage Pass storage.
 */
void order_spawn_components(Storage& storage) noexcept {
    const auto first = storage.spawnComponents.begin();
    const auto last = first + static_cast<std::ptrdiff_t>(storage.spawnComponentCount);
    std::sort(first, last, [](const SpawnComponent& row, const SpawnComponent& other) noexcept {
        return row.tag < other.tag;
    });
}

/**
 * Finds the spawn component one member tag names.
 * @param storage Pass storage holding the spawn components in tag order.
 * @param tag Container member tag.
 * @param entryIndex Receives the set-tag row that component wraps.
 * @return True when the member is a component wrapping a swept spawn set.
 */
[[nodiscard]] bool
find_spawn_component(const Storage& storage, std::uint32_t tag, std::size_t& entryIndex) noexcept {
    entryIndex = 0;
    const auto first = storage.spawnComponents.begin();
    const auto last = first + static_cast<std::ptrdiff_t>(storage.spawnComponentCount);
    const auto found = std::lower_bound(
        first, last, tag, [](const SpawnComponent& row, std::uint32_t value) noexcept {
            return row.tag < value;
        });
    if (found == last || found->tag != tag) {
        return false;
    }
    entryIndex = found->entryIndex;
    return true;
}

/**
 * Records one component that wraps a swept spawn set.
 * @param storage Pass storage.
 * @param tag Component tag.
 * @param blob Whole component bytes.
 * @return True unless the component list is full.
 */
[[nodiscard]] bool
consume_component(Storage& storage, std::uint32_t tag, std::span<const std::byte> blob) noexcept {
    std::uint32_t resource = 0;
    std::size_t entryIndex = 0;
    // Components wrap every kind of resource, so one that names something else is ordinary.
    if (!tables::component_resource(blob, resource) || !find_entry(storage, resource, entryIndex)) {
        return true;
    }
    if (storage.spawnComponentCount >= storage.spawnComponents.size()) {
        return false;
    }
    storage.spawnComponents[storage.spawnComponentCount++] = {
        tag, static_cast<std::uint16_t>(entryIndex)};
    return true;
}

/**
 * Applies one container's bubble mask to every spawn set it holds.
 * @param storage Pass storage.
 * @param blob Whole container bytes.
 * @return True unless the container is malformed.
 */
[[nodiscard]] bool consume_container(Storage& storage, std::span<const std::byte> blob) noexcept {
    BubbleMask mask{};
    tables::Array members{};
    // A container with no mask or no member array holds nothing this pass needs.
    if (!tables::container_bubble_mask(blob, mask.bytes)
        || !tables::container_members(blob, members)
        || (members.count != 0 && members.elementClass != tables::kContainerMemberClass)) {
        return true;
    }
    for (std::uint64_t index = 0; index < members.count; ++index) {
        std::uint32_t member = 0;
        if (!tables::container_member_at(blob, members, index, member)) {
            break;
        }
        std::size_t entryIndex = 0;
        if (!find_spawn_component(storage, member, entryIndex)) {
            continue;
        }
        BubbleMask& target = storage.tagMasks[entryIndex];
        for (std::size_t byte = 0; byte < target.bytes.size(); ++byte) {
            target.bytes[byte] |= mask.bytes[byte];
        }
        storage.tagBound[entryIndex] = 1;
    }
    return true;
}

/**
 * Parses and folds in one whole spawn-set blob.
 * @param storage Pass storage.
 * @param entry Scanned tag and owning stem.
 * @param blob Whole spawn-set bytes.
 * @return True when every point is canonical and fits the catalogue.
 */
[[nodiscard]] bool
consume(Storage& storage, const Entry& entry, std::span<const std::byte> blob) noexcept {
    tables::Array points{};
    // A set whose descriptor is missing, or whose array holds another element class, carries no
    // spawn point for this stem. It is skipped, never fatal: the packages hold such sets and
    // failing on one would drop the whole catalogue.
    if (!tables::spawn_points(blob, points)
        || (points.count != 0 && points.elementClass != tables::kSpawnPointClass)) {
        return true;
    }
    if (points.count
        > (std::numeric_limits<std::uint32_t>::max)() - storage.stems[entry.stemIndex].pointCount) {
        return false;
    }
    std::uint32_t recorded = 0;
    const std::size_t entryIndex = static_cast<std::size_t>(&entry - storage.entries.data());
    for (std::uint64_t index = 0; index < points.count; ++index) {
        tables::SpawnPoint point{};
        // A point past the end of the blob ends this set. Its array header claimed more than the
        // entry holds, which says nothing about the sets around it.
        if (!tables::spawn_point_at(blob, points, static_cast<std::size_t>(index), point)) {
            break;
        }
        // A full accumulator is a capacity limit, not bad data, and no later set can fit either.
        if (!record_point(storage, entry.stemIndex, point.nameHash, entryIndex)) {
            return false;
        }
        ++recorded;
    }
    storage.stems[entry.stemIndex].pointCount += recorded;
    return true;
}

/** What one bounded batch of one stage produced. */
enum class BatchResult {
    pending,
    done,
    invalid,
};

/**
 * Reads one bounded batch of tags and hands each blob to a consumer.
 * @tparam Consumer Callable taking the pass, the tag and its blob, returning false when fatal.
 * @param storage Pass storage carrying the skip counter.
 * @param reader Injected tag reader.
 * @param prefetch Injected read-ahead, or null when the reader has none.
 * @param context Reader context.
 * @param tags Tags of this stage.
 * @param cursor Tag the batch resumes from, advanced here.
 * @param reads Reads already spent in this call, advanced here.
 * @param budget Maximum reads for the whole call.
 * @param skipUnreadable True once the caller's read window has closed.
 * @param consume Consumer applied to every blob that read.
 * @return Done when the stage is finished, pending when it is not, invalid when the data is
 * unusable.
 */
template <typename Consumer>
[[nodiscard]] BatchResult read_batch(Storage& storage,
                                     BlobReader reader,
                                     BlobPrefetch prefetch,
                                     void* context,
                                     std::span<const std::uint32_t> tags,
                                     std::size_t& cursor,
                                     std::size_t& reads,
                                     std::size_t budget,
                                     bool skipUnreadable,
                                     Consumer consume) noexcept {
    // The whole window is asked for at once, then parsed one blob at a time exactly as before.
    // Whether the read-ahead actually held a blob changes nothing here: the reader answers either
    // way, so the stage reads the same tags in the same order.
    if (prefetch != nullptr && cursor < tags.size() && reads < budget) {
        const std::size_t window = (std::min)(budget - reads, tags.size() - cursor);
        prefetch(context, tags.subspan(cursor, window));
    }
    while (cursor < tags.size() && reads < budget) {
        std::span<const std::byte> blob{};
        ++reads;
        if (!reader(context, tags[cursor], blob)) {
            // Block keys resolve during the boot, so an early read failure is ordinary and the
            // stage waits. Once the window has closed the tag is dropped instead.
            if (!skipUnreadable) {
                return BatchResult::pending;
            }
            ++storage.skipped;
            ++cursor;
            continue;
        }
        if (!consume(storage, tags[cursor], blob)) {
            return BatchResult::invalid;
        }
        ++cursor;
    }
    return cursor == tags.size() ? BatchResult::done : BatchResult::pending;
}

/**
 * Converts one unfinished stage result into the pass result.
 * @param storage Pass storage, marked invalid when the stage was.
 * @param batch Stage result that is not done.
 * @return The matching pass result.
 */
[[nodiscard]] AdvanceResult result_of(Storage& storage, BatchResult batch) noexcept {
    if (batch != BatchResult::invalid) {
        return AdvanceResult::pending;
    }
    storage.invalid = true;
    return AdvanceResult::invalid;
}

/** @param storage Completed accumulator. @return True when its flat bank is canonical. */
[[nodiscard]] bool finalize(Storage& storage) noexcept {
    storage.nameHashCount = 0;
    for (const HashSlot& slot : storage.accumulator) {
        if (slot.occupied) {
            if (storage.nameHashCount >= storage.nameHashes.size()) {
                return false;
            }
            storage.nameHashes[storage.nameHashCount++] = {slot.value,
                                                           slot.pointCount,
                                                           slot.stemIndex,
                                                           slot.mask.bytes,
                                                           slot.unbound,
                                                           slot.inMapPackage,
                                                           slot.activityPackageCount,
                                                           slot.activityPackageOverflow,
                                                           slot.activityPackages};
        }
    }
    auto end = storage.nameHashes.begin() + static_cast<std::ptrdiff_t>(storage.nameHashCount);
    std::sort(storage.nameHashes.begin(), end, hash_less);
    std::size_t cursor = 0;
    for (std::size_t stemIndex = 0; stemIndex < storage.stemCount; ++stemIndex) {
        spawn_state::Stem& stem = storage.stems[stemIndex];
        stem.nameHashOffset = static_cast<std::uint16_t>(cursor);
        while (cursor < storage.nameHashCount
               && storage.nameHashes[cursor].stemIndex == stemIndex) {
            ++stem.nameHashCount;
            ++cursor;
        }
    }
    storage.finalized =
        cursor == storage.nameHashCount
        && spawn_state::valid(std::span(storage.stems).first(storage.stemCount),
                              std::span(storage.nameHashes).first(storage.nameHashCount));
    return storage.finalized;
}

} // namespace

/** Clears one extraction pass without freeing process-owned outer storage. */
void reset(Storage& storage) noexcept {
    storage.entries.fill(Entry{});
    storage.entryCount = 0;
    storage.tagOrder.fill(0);
    storage.tagMasks.fill(BubbleMask{});
    storage.tagBound.fill(0);
    storage.componentTags.fill(0);
    storage.componentCount = 0;
    storage.containerTags.fill(0);
    storage.containerCount = 0;
    storage.spawnComponents.fill(SpawnComponent{});
    storage.spawnComponentCount = 0;
    storage.componentsOrdered = false;
    storage.componentCursor = 0;
    storage.containerCursor = 0;
    storage.stems.fill(spawn_state::Stem{});
    storage.stemCount = 0;
    storage.accumulator.fill(HashSlot{});
    storage.accumulatedCount = 0;
    storage.nameHashes.fill(spawn_state::NameHash{});
    storage.nameHashCount = 0;
    storage.cursor = 0;
    storage.skipped = 0;
    storage.collected = false;
    storage.finalized = false;
    storage.invalid = false;
}

/** Converts one package family into the map stem its spawn-set tags are grouped under. */
bool normalize_stem(std::wstring_view packageFamily,
                    std::span<char> output,
                    std::uint8_t& length) noexcept {
    length = 0;
    std::fill(output.begin(), output.end(), '\0');
    if (packageFamily.empty()) {
        return false;
    }
    std::wstring_view stem = packageFamily;
    /** Installed package-family grammar used by the package path index. */
    constexpr std::wstring_view kPrefix = L"w64_";
    constexpr std::wstring_view kActivities = L"_activities";
    constexpr std::wstring_view kUnpacked = L"_unp";
    constexpr std::size_t kPackageIdDigits = 4;
    if (stem.starts_with(kPrefix) && stem.size() > kPrefix.size() + kPackageIdDigits + 1U) {
        const std::size_t idSeparator = stem.size() - kPackageIdDigits - 1U;
        const bool packageId =
            stem[idSeparator] == L'_'
            && std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(idSeparator + 1U),
                           stem.end(),
                           hexadecimal);
        if (packageId) {
            stem = stem.substr(kPrefix.size(), idSeparator - kPrefix.size());
            if (stem.ends_with(kActivities)) {
                stem.remove_suffix(kActivities.size());
            } else {
                const std::size_t suffix = stem.rfind(kUnpacked);
                if (suffix != std::wstring_view::npos && suffix + kUnpacked.size() < stem.size()
                    && std::all_of(stem.begin()
                                       + static_cast<std::ptrdiff_t>(suffix + kUnpacked.size()),
                                   stem.end(),
                                   [](wchar_t value) { return value >= L'0' && value <= L'9'; })) {
                    stem = stem.substr(0, suffix);
                }
            }
        }
    }
    if (stem.empty() || stem.size() > output.size()
        || stem.size() > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    for (std::size_t index = 0; index < stem.size(); ++index) {
        output[index] = lowercase_ascii(stem[index]);
        if (output[index] == '\0') {
            std::fill(output.begin(), output.end(), '\0');
            return false;
        }
    }
    length = static_cast<std::uint8_t>(stem.size());
    return true;
}

/** Tells a map package from an activity one. */
bool activity_family(std::wstring_view packageFamily) noexcept {
    /** Package families that hold one activity's own content carry this before the package id. */
    constexpr std::wstring_view kActivities = L"_activities";
    /** The package id and its separator follow the marker. */
    constexpr std::size_t kPackageIdSuffix = 5;
    const std::size_t marker = packageFamily.rfind(kActivities);
    return marker != std::wstring_view::npos
           && marker + kActivities.size() + kPackageIdSuffix == packageFamily.size();
}

/** Adds one class-scan match to an unfinished pass. */
bool add_entry(Storage& storage, const package_reader::ClassEntry& entry) noexcept {
    if (storage.collected || storage.invalid || entry.tag == 0
        || storage.entryCount >= storage.entries.size()) {
        return false;
    }
    Entry& output = storage.entries[storage.entryCount];
    if (!normalize_stem(entry.packageFamily, output.stem, output.stemLength)) {
        return false;
    }
    output.tag = entry.tag;
    output.packageId = tables::package_of(entry.tag);
    output.activityPackage = activity_family(entry.packageFamily) ? 1U : 0U;
    ++storage.entryCount;
    return true;
}

/** Adds one component tag the class sweep found. */
bool add_component(Storage& storage, std::uint32_t tag) noexcept {
    if (storage.collected || storage.invalid || tag == 0
        || storage.componentCount >= storage.componentTags.size()) {
        return false;
    }
    storage.componentTags[storage.componentCount++] = tag;
    return true;
}

/** Adds one container tag the class sweep found. */
bool add_container(Storage& storage, std::uint32_t tag) noexcept {
    if (storage.collected || storage.invalid || tag == 0
        || storage.containerCount >= storage.containerTags.size()) {
        return false;
    }
    storage.containerTags[storage.containerCount++] = tag;
    return true;
}

/** Sorts the scanned tags and builds their canonical stem groups. */
bool finish_collection(Storage& storage) noexcept {
    if (storage.collected || storage.invalid || storage.entryCount == 0) {
        return false;
    }
    std::array<std::uint32_t, kTagCapacity> tags{};
    for (std::size_t index = 0; index < storage.entryCount; ++index) {
        tags[index] = storage.entries[index].tag;
    }
    auto tagEnd = tags.begin() + static_cast<std::ptrdiff_t>(storage.entryCount);
    std::sort(tags.begin(), tagEnd);
    if (std::adjacent_find(tags.begin(), tagEnd) != tagEnd) {
        storage.invalid = true;
        return false;
    }
    auto end = storage.entries.begin() + static_cast<std::ptrdiff_t>(storage.entryCount);
    std::sort(storage.entries.begin(), end, entry_less);
    for (std::size_t index = 0; index < storage.entryCount; ++index) {
        Entry& entry = storage.entries[index];
        const bool newStem = storage.stemCount == 0
                             || stem_of(storage.stems[storage.stemCount - 1]) != stem_of(entry);
        if (newStem) {
            if (storage.stemCount >= storage.stems.size()) {
                storage.invalid = true;
                return false;
            }
            spawn_state::Stem& stem = storage.stems[storage.stemCount++];
            std::copy(entry.stem.begin(), entry.stem.end(), stem.name.begin());
            stem.nameLength = entry.stemLength;
        }
        entry.stemIndex = static_cast<std::uint16_t>(storage.stemCount - 1U);
        ++storage.stems[entry.stemIndex].setCount;
    }
    for (std::size_t index = 0; index < storage.entryCount; ++index) {
        storage.tagOrder[index] = static_cast<std::uint16_t>(index);
    }
    auto orderEnd = storage.tagOrder.begin() + static_cast<std::ptrdiff_t>(storage.entryCount);
    std::sort(storage.tagOrder.begin(),
              orderEnd,
              [&storage](std::uint16_t firstRow, std::uint16_t secondRow) noexcept {
                  return storage.entries[firstRow].tag < storage.entries[secondRow].tag;
              });
    storage.collected = true;
    return true;
}

/** Reads and aggregates the next bounded batch of blobs. */
AdvanceResult advance(Storage& storage,
                      BlobReader reader,
                      BlobPrefetch prefetch,
                      void* context,
                      std::size_t budget,
                      bool skipUnreadable) noexcept {
    if (storage.invalid || !storage.collected || reader == nullptr || budget == 0) {
        return AdvanceResult::invalid;
    }
    if (storage.finalized) {
        return AdvanceResult::complete;
    }
    std::size_t reads = 0;
    const BatchResult components =
        read_batch(storage,
                   reader,
                   prefetch,
                   context,
                   std::span(storage.componentTags).first(storage.componentCount),
                   storage.componentCursor,
                   reads,
                   budget,
                   skipUnreadable,
                   [](Storage& pass, std::uint32_t tag, std::span<const std::byte> blob) noexcept {
                       return consume_component(pass, tag, blob);
                   });
    if (components != BatchResult::done) {
        return result_of(storage, components);
    }
    // Every component has been read by here, so the order is final and built once.
    if (!storage.componentsOrdered) {
        order_spawn_components(storage);
        storage.componentsOrdered = true;
    }
    const BatchResult containers =
        read_batch(storage,
                   reader,
                   prefetch,
                   context,
                   std::span(storage.containerTags).first(storage.containerCount),
                   storage.containerCursor,
                   reads,
                   budget,
                   skipUnreadable,
                   [](Storage& pass, std::uint32_t, std::span<const std::byte> blob) noexcept {
                       return consume_container(pass, blob);
                   });
    if (containers != BatchResult::done) {
        return result_of(storage, containers);
    }
    while (storage.cursor < storage.entryCount && reads < budget) {
        const Entry& entry = storage.entries[storage.cursor];
        std::span<const std::byte> blob{};
        ++reads;
        if (!reader(context, entry.tag, blob)) {
            // Block keys resolve during the boot, so an early read failure is ordinary and the
            // pass waits. Once the window has closed the tag is dropped instead, because a pass
            // that waits forever is one the whole content pipeline waits behind.
            if (!skipUnreadable) {
                return AdvanceResult::pending;
            }
            ++storage.skipped;
            ++storage.cursor;
            continue;
        }
        if (!consume(storage, entry, blob)) {
            storage.invalid = true;
            return AdvanceResult::invalid;
        }
        ++storage.cursor;
    }
    if (storage.cursor != storage.entryCount) {
        return AdvanceResult::pending;
    }
    if (!finalize(storage)) {
        storage.invalid = true;
        return AdvanceResult::invalid;
    }
    return AdvanceResult::complete;
}

} // namespace sunrise::client::content::spawn_sets
