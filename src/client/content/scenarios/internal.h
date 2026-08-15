#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "../../../state/build_data/scenarios/definition.h"

namespace sunrise::client::content::scenarios {

namespace layouts = state::build_data::scenarios;
namespace reader = middleware::content::packages::reader;

/**
 * Placed-object tags the memo holds. The walk reaches 5,826 distinct objects over the installed
 * packages, and the table needs headroom to stay a cheap open-addressed probe.
 */
inline constexpr std::size_t kObjectMemoCapacity = 16'384;
/** Memo value for an object that declares no roster slot type. */
inline constexpr std::uint16_t kNotARosterGroup = 0xFFFF;
/** Slot types run from 1 through the widest the packages declare. */
inline constexpr std::size_t kSlotTypeSpan = layouts::kMaximumSlotType + 1;

/** One memo row: a placed-object tag and the roster group it produced. */
struct ObjectMemo {
    std::uint32_t tag{};
    std::uint16_t group{kNotARosterGroup};
};

/** Fixed working storage for one roster pass, kept off the caller stack. */
struct RosterStorage {
    std::vector<std::byte> scenario;
    std::vector<std::byte> entry;
    std::vector<std::byte> registry;
    std::vector<std::byte> object;
    std::vector<std::byte> chain;
    std::array<ObjectMemo, kObjectMemoCapacity> memo{};
    std::array<layouts::RosterGroup, layouts::kRosterGroupCapacity> groups{};
    std::size_t groupCount{};
    /** Slot flags per slot type, read from a group object's descriptor chain. */
    std::array<std::uint8_t, kSlotTypeSpan> slotFlags{};
    std::array<std::uint8_t, kSlotTypeSpan> slotFlagsKnown{};
    /** Group objects whose descriptor chain did not give every slot type they declare. */
    std::size_t unresolvedGroups{};
    /** Destinations walked so far. The walk resumes here on the next call. */
    std::size_t cursor{};
    /** Tag reads spent in the current call, which is what bounds how long it blocks. */
    std::size_t reads{};
};

/** Tag-read budget bounds one process-freeze interval and keeps worker shutdown responsive. */
inline constexpr std::size_t kRosterReadBudget = 150;

/** Live scenario tags found by the class sweep. The measured live count is 468. */
inline constexpr std::size_t kLiveTagCapacity = 1'024;
/**
 * How long the collection keeps retrying the destinations that have not read yet.
 * Packages register during the boot, so an early attempt reads fewer of them. One run latched at
 * 417 of 466 and the destination it dropped was the Tower.
 */
inline constexpr std::uint64_t kResolveWindowMs = 15'000;
/** Tag reads one collection call may spend, for the same reason the roster walk is bounded. */
inline constexpr std::size_t kResolveReadBudget = 150;

/** One live scenario tag and the map-package stem of the package that carries it. */
struct LiveTag {
    std::uint32_t tag{};
    std::array<char, layouts::kSpawnStemCapacity> stem{};
    std::uint8_t stemLength{};
};

/** One pass of fixed storage, kept off the caller stack. */
struct Storage {
    std::array<LiveTag, kLiveTagCapacity> liveTags{};
    std::size_t liveTagCount{};
    std::array<layouts::Definition, layouts::kDefinitionCapacity> rows{};
    std::size_t rowCount{};
    /** Patch index each row's tag came from, so a later patch replaces an earlier one. */
    std::array<std::uint32_t, layouts::kDefinitionCapacity> rowPatch{};
    /** One byte per row: set once its bubble layout has read. */
    std::array<std::uint8_t, layouts::kDefinitionCapacity> resolved{};
    std::size_t resolvedCount{};
    /** Rows whose tag the class sweep still carries. Only these can ever read. */
    std::size_t liveRowCount{};
    /** Where the next resolve pass starts, so every pending row is retried in turn. */
    std::size_t resolveCursor{};
    /** Tick after which the collection stops waiting for the rows that have not read. */
    std::uint64_t resolveDeadlineTick{};
    std::vector<std::byte> blob{};
    RosterStorage roster{};
    /** Resolved rows, moved to the front. The roster walk runs over exactly these. */
    std::size_t keptCount{};
    /** Retried rounds of the resolve window, so a boot that never reads reports it once. */
    std::uint32_t resolveRounds{};
    /** Set once the sweep and the name match are done, so they run once per boot. */
    bool collected{};
    /** Set once the resolved rows are compacted and the roster walk may start. */
    bool compacted{};
};

/**
 * Sweeps the installed packages for scenario tags and matches them to destination names.
 * @param source Package directory and borrowed block keys.
 * @param storage Pass storage receiving the live tags and the named rows.
 * @param reason Receives the step that refused, or stays null.
 * @return True when both steps finished.
 */
[[nodiscard]] bool
collect_rows(const reader::Source& source, Storage& storage, const char*& reason) noexcept;

/**
 * Reads the bubble layout of rows that have not read yet, within this call's budget.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage carrying the resolve cursor.
 * @return True when the collection has settled and may be compacted.
 */
[[nodiscard]] bool
resolve_pending(const reader::Source& source, reader::Scratch& scratch, Storage& storage) noexcept;

/**
 * Moves every resolved row to the front of the row array.
 * @param storage Pass storage whose kept count is set here.
 */
void compact_rows(Storage& storage) noexcept;

/**
 * Re-arms the resolve window so a pass that read nothing tries the whole row set again.
 * @param storage Pass storage whose resolve state is cleared.
 */
void rearm_resolve(Storage& storage) noexcept;

/** One candidate group of one destination, with what its publish order is sorted on. */
struct Candidate {
    std::uint16_t group{};
    std::uint32_t key{};
    bool bindsPlayer{};
    bool reportsLifetime{};
    bool primaryRegistry{};
};

/** @return True when both groups carry the same registry key and full wire slot layout. */
[[nodiscard]] constexpr bool same_group_layout(const layouts::RosterGroup& left,
                                               const layouts::RosterGroup& right) noexcept {
    if (left.registryKey != right.registryKey || left.slotCount != right.slotCount) {
        return false;
    }
    for (std::size_t slot = 0; slot < left.slotCount; ++slot) {
        if (left.slotTypes[slot] != right.slotTypes[slot]
            || left.slotFlags[slot] != right.slotFlags[slot]) {
            return false;
        }
    }
    return true;
}

/** Everything one destination's walk builds up. */
struct Walk {
    middleware::content::packages::tables::RosterIntersection intersection{};
    std::array<Candidate, middleware::content::packages::tables::kRosterKeyCapacity> candidates{};
    std::size_t candidateCount{};
};

/**
 * Keeps the candidates whose key is in every slice set and writes them into the destination row.
 * @param walk Accumulator for one destination.
 * @param row Destination row receiving its group indices.
 */
void publish_safe(Walk& walk, layouts::Definition& row) noexcept;

/**
 * Finds the roster group of one placed object, reading it only the first time it is seen.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param objectTag Tag from an object registry.
 * @param group Receives the roster group index, or the not-a-group sentinel.
 * @return True when the object was read or was already known.
 */
[[nodiscard]] bool resolve_object(const reader::Source& source,
                                  reader::Scratch& scratch,
                                  RosterStorage& storage,
                                  std::uint32_t objectTag,
                                  std::uint16_t& group) noexcept;

/**
 * Walks the next batch of destination rows for their roster groups.
 * One call spends at most the read budget and then returns, so the pass resumes across calls.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage carrying the cursor between calls.
 * @param rows Destination rows whose tag is already set, updated in place.
 * @return True when every row has been walked.
 */
[[nodiscard]] bool build_rosters(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 RosterStorage& storage,
                                 std::span<layouts::Definition> rows) noexcept;

} // namespace sunrise::client::content::scenarios
