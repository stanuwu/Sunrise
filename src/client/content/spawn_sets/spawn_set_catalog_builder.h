#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/spawn_sets/definition.h"

namespace sunrise::client::content::spawn_sets {

namespace spawn_state = state::build_data::spawn_sets;
namespace package_reader = middleware::content::packages::reader;

/** Spawn-set tags the class sweep finds. The measured live count is 386. */
inline constexpr std::size_t kTagCapacity = 1'024;
/** Component tags the class sweep finds. The measured live count is 14,838. */
inline constexpr std::size_t kComponentCapacity = 20'480;
/** Container tags the class sweep finds. The measured live count is 5,516. */
inline constexpr std::size_t kContainerCapacity = 8'192;
/** Components that wrap a spawn set. Every installed set has one, and a few have two. */
inline constexpr std::size_t kSpawnComponentCapacity = 2'048;
/**
 * Slots of the open-addressed accumulator that counts points per stem and name hash.
 * The measured distinct-hash count is 2,090, and a power of two keeps the probe a mask.
 */
inline constexpr std::size_t kAccumulatorCapacity = 8'192;
/**
 * Tag reads one slice may spend, which the read-ahead fetches as one run.
 * The slice waits for every reader, so this bounds how long it blocks the frame. At the measured
 * cost of a read that is a few tens of milliseconds.
 */
inline constexpr std::size_t kReadBudget = 512;
/**
 * How long the pass keeps waiting for a tag that will not read.
 * Block keys resolve during the boot, so an early read fails for a reason that fixes itself. After
 * this the tag is dropped, because the whole content pipeline waits behind this pass.
 */
inline constexpr std::uint64_t kReadWindowMs = 15'000;

/** One scanned spawn-set tag and the map-package stem that owns it. */
struct Entry {
    std::uint32_t tag{};
    std::array<char, spawn_state::kStemNameCapacity> stem{};
    std::uint16_t stemIndex{};
    std::uint8_t stemLength{};
    /** Package the tag lives in, which decides whether a destination streams it. */
    std::uint16_t packageId{};
    /** One when that package is an activity package rather than a map one. */
    std::uint8_t activityPackage{};
};

/** The bubbles one container names, as one bit per map-global bubble index. */
struct BubbleMask {
    std::array<std::uint8_t, spawn_state::kBubbleMaskBytes> bytes{};
};

/** One component that wraps a spawn set, and the set-tag row it wraps. */
struct SpawnComponent {
    std::uint32_t tag{};
    std::uint16_t entryIndex{};
};

/** One accumulator slot: a stem, a name hash, and the points carrying it. */
struct HashSlot {
    std::uint32_t value{};
    std::uint32_t pointCount{};
    std::uint16_t stemIndex{};
    bool occupied{};
    /** Union of the bubble masks of every set tag of this stem declaring the hash. */
    BubbleMask mask{};
    /** One when some tag declaring the hash has no owning container. */
    std::uint8_t unbound{};
    std::uint8_t inMapPackage{};
    std::uint8_t activityPackageCount{};
    std::uint8_t activityPackageOverflow{};
    std::array<std::uint16_t, spawn_state::kPackageCapacity> activityPackages{};
};

/** Fixed working storage for one extraction pass, kept off the caller stack. */
struct Storage {
    std::array<Entry, kTagCapacity> entries{};
    std::size_t entryCount{};
    /** Set-tag rows in ascending tag order, so a wrapped tag is found by search. */
    std::array<std::uint16_t, kTagCapacity> tagOrder{};
    /** Bubbles each set tag is offered in, from the container that holds its component. */
    std::array<BubbleMask, kTagCapacity> tagMasks{};
    /** One per set tag whose component a container holds. The rest carry no mask. */
    std::array<std::uint8_t, kTagCapacity> tagBound{};
    std::array<std::uint32_t, kComponentCapacity> componentTags{};
    std::size_t componentCount{};
    std::array<std::uint32_t, kContainerCapacity> containerTags{};
    std::size_t containerCount{};
    std::array<SpawnComponent, kSpawnComponentCapacity> spawnComponents{};
    std::size_t spawnComponentCount{};
    /** Component the next batch resumes from. */
    std::size_t componentCursor{};
    /** Container the next batch resumes from, read only once every component has. */
    std::size_t containerCursor{};
    std::array<spawn_state::Stem, spawn_state::kStemCapacity> stems{};
    std::size_t stemCount{};
    std::array<HashSlot, kAccumulatorCapacity> accumulator{};
    std::size_t accumulatedCount{};
    std::array<spawn_state::NameHash, spawn_state::kNameHashCapacity> nameHashes{};
    std::size_t nameHashCount{};
    /** Entry the next batch resumes from, so the pass continues across calls. */
    std::size_t cursor{};
    /** Entries whose blob never read inside the window, so the pass moved past them. */
    std::size_t skipped{};
    /** Tick the pass started on, so every report says what the pass cost. */
    std::uint64_t startedTick{};
    /** Set once the spawn components are in tag order, which the member lookup needs. */
    bool componentsOrdered{};
    /** Set once the sweep and its stem grouping are done, so they run once per boot. */
    bool collected{};
    /** Set once the flat bank is built and checked. */
    bool finalized{};
    /** Set when the pass found data it cannot represent, which no retry can fix. */
    bool invalid{};
};

/** What one bounded batch of blob reads produced. */
enum class AdvanceResult {
    pending,
    complete,
    invalid,
};

/** Injected tag reader. The blob stays live until the next call. */
using BlobReader = bool (*)(void* context,
                            std::uint32_t tag,
                            std::span<const std::byte>& blob) noexcept;

/**
 * Injected read-ahead for a run of tags the pass is about to ask for, in order.
 * A blob waits out the disk before it decodes, so fetching a run at once hides most of that
 * wait. The pass still parses one blob at a time, on its own thread. No read-ahead means null.
 */
using BlobPrefetch = void (*)(void* context, std::span<const std::uint32_t> tags) noexcept;

/** Clears one extraction pass without freeing process-owned outer storage. */
void reset(Storage& storage) noexcept;

/**
 * Converts one package family into the map stem its spawn-set tags are grouped under.
 * @param packageFamily Leaf name without patch suffix or extension.
 * @param output Receives lowercase ASCII stem bytes.
 * @param length Receives the stem length.
 * @return True when the package family has a usable canonical stem.
 */
[[nodiscard]] bool normalize_stem(std::wstring_view packageFamily,
                                  std::span<char> output,
                                  std::uint8_t& length) noexcept;

/**
 * Tells a map package from an activity one.
 * A destination always loads its map packages, and loads an activity package only when its own
 * slice-set entries name it.
 * @param packageFamily Leaf name without patch suffix or extension.
 * @return True when the family is an activity package.
 */
[[nodiscard]] bool activity_family(std::wstring_view packageFamily) noexcept;

/**
 * Adds one class-scan match to an unfinished pass.
 * @param storage Pass storage.
 * @param entry Scan match carrying its package family.
 * @return False when the pass is finished, the stem is unusable, or storage is full.
 */
[[nodiscard]] bool add_entry(Storage& storage, const package_reader::ClassEntry& entry) noexcept;

/**
 * Adds one component tag the class sweep found.
 * @param storage Pass storage.
 * @param tag Component tag.
 * @return False when the pass is finished or storage is full.
 */
[[nodiscard]] bool add_component(Storage& storage, std::uint32_t tag) noexcept;

/**
 * Adds one container tag the class sweep found.
 * @param storage Pass storage.
 * @param tag Container tag.
 * @return False when the pass is finished or storage is full.
 */
[[nodiscard]] bool add_container(Storage& storage, std::uint32_t tag) noexcept;

/**
 * Sorts the scanned tags and builds their canonical stem groups.
 * @param storage Pass storage.
 * @return True when the tags are unique and every stem fits.
 */
[[nodiscard]] bool finish_collection(Storage& storage) noexcept;

/**
 * Reads and aggregates the next bounded batch of spawn-set blobs.
 * @param storage Pass storage carrying its cursor and accumulators.
 * @param reader Injected tag reader.
 * @param prefetch Injected read-ahead, or null when the reader has none.
 * @param context Reader context.
 * @param budget Maximum tag reads in this call.
 * @param skipUnreadable True once the caller's read window has closed, which moves the cursor past
 *        a tag that will not read instead of waiting for it again.
 * @return Pending, complete, or invalid without partial publication.
 */
[[nodiscard]] AdvanceResult advance(Storage& storage,
                                    BlobReader reader,
                                    BlobPrefetch prefetch,
                                    void* context,
                                    std::size_t budget,
                                    bool skipUnreadable) noexcept;

} // namespace sunrise::client::content::spawn_sets
