#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <tuple>
#include <vector>

#include "../merge/internal.h"
#include "../reader/layout.h"
#include "sidecar.h"
#include "table_merge.h"

namespace sunrise::middleware::content::packages::table_merge::internal {

using merge::internal::align;
using merge::internal::Cursor;
using merge::internal::load;
using merge::internal::store;
using reader::layout::has_size_word;
using reader::layout::tag_of;

/** Edits clash per aligned 8-byte word, so no field of 8 bytes or less mixes two mods. */
inline constexpr std::uint32_t kWordSize = 8;
/** Authored array headers and added entry regions start on 16-byte boundaries. */
inline constexpr std::uint64_t kRowAlignment = 16;

/** @return The entry description one input carries, or null without merge data. */
[[nodiscard]] inline const Entry* entry_of(const Input& input, std::uint32_t entryIndex) noexcept {
    return input.sidecar == nullptr ? nullptr : input.sidecar->entry(entryIndex);
}

/** The merged entry being built. */
struct Frame final {
    /** Proven stock bytes, [0, stockSize). */
    std::span<const std::byte> stock;
    std::vector<std::byte>* output{};
    /** Per input: where its added region now starts, minus stockSize. */
    std::vector<std::int64_t> blobDelta;
};

/** A stock range the byte merge leaves to the row engine. */
struct Span final {
    std::uint32_t begin{};
    std::uint32_t end{};
    /** A descriptor, a header count or the size word, rather than an array. */
    bool structural{};
};

/** Rows to add to each mod's site values, per target. Absent means 0. */
class Shifts final {
public:
    /** Sets one shift. Throws std::bad_alloc. */
    void set(std::size_t mod, std::uint32_t tag, std::uint32_t descriptor, std::uint32_t rows) {
        const Key key{mod, tag, descriptor};
        const auto at = std::lower_bound(rows_.begin(), rows_.end(), key, before);
        if (at != rows_.end() && same(*at, key)) {
            at->rows = rows;
            return;
        }
        rows_.insert(at, Row{mod, tag, descriptor, rows});
    }

    /** @return The shift for one mod and target, or 0. */
    [[nodiscard]] std::uint32_t
    get(std::size_t mod, std::uint32_t tag, std::uint32_t descriptor) const noexcept {
        const Key key{mod, tag, descriptor};
        const auto at = std::lower_bound(rows_.begin(), rows_.end(), key, before);
        return at != rows_.end() && same(*at, key) ? at->rows : 0;
    }

    void clear() noexcept {
        rows_.clear();
    }

private:
    struct Key final {
        std::size_t mod{};
        std::uint32_t tag{};
        std::uint32_t descriptor{};
    };
    struct Row final {
        std::size_t mod{};
        std::uint32_t tag{};
        std::uint32_t descriptor{};
        std::uint32_t rows{};
    };

    [[nodiscard]] static bool before(const Row& row, const Key& key) noexcept {
        return std::tie(row.mod, row.tag, row.descriptor)
               < std::tie(key.mod, key.tag, key.descriptor);
    }
    [[nodiscard]] static bool same(const Row& row, const Key& key) noexcept {
        return row.mod == key.mod && row.tag == key.tag && row.descriptor == key.descriptor;
    }

    std::vector<Row> rows_;
};

/** Merges the stock-region bytes the row engine does not own; gets its array and structure spans.
 */
using SpanMerge = std::function<Status(std::span<const Span> spans)>;

/**
 * Reads every op of one entry once, lets `bytes` merge around their stock spans, then rebuilds
 * each op at the end of the output and rewrites its descriptors.
 * Conflicts it adds carry the entry index and mods; the caller fills the package id.
 * @param frame Stock bytes, output and added-region moves.
 * @param shiftedCopies Each input's bytes with its sites shifted.
 * @param inputs The entry's inputs, in load order.
 * @param entryIndex Entry being merged.
 * @param bytes Byte merge, run after the ops are read and before any row is written.
 * @param conflicts Receives row bytes two mods changed differently.
 * @return ok, the byte merge's failure, or why the rows cannot merge.
 */
[[nodiscard]] Status merge_ops(Frame& frame,
                               std::span<const std::vector<std::byte>> shiftedCopies,
                               std::span<const Input> inputs,
                               std::uint32_t entryIndex,
                               const SpanMerge& bytes,
                               std::vector<Conflict>& conflicts);

/**
 * Adds the planned shift to every site of one copy.
 * @param input Mod copy and its sidecar.
 * @param entryIndex Entry the copy holds.
 * @param shifts Shifts for target sites.
 * @param opLocalShifts Shift per op of this entry, for op-local sites.
 * @param copy Copy of the input bytes; sites are rewritten in place.
 * @return ok, or limit when a value no longer fits its width or sign.
 */
[[nodiscard]] Status shift_sites(const Input& input,
                                 std::uint32_t entryIndex,
                                 const Shifts& shifts,
                                 std::span<const std::uint32_t> opLocalShifts,
                                 std::vector<std::byte>& copy);

/**
 * Checks that every site of one copy names a row its mod added.
 * @return ok, or sites when a value lies outside [stockCount, modCount).
 */
[[nodiscard]] Status check_sites(const Input& input, std::uint32_t entryIndex) noexcept;

/**
 * Adds every group entry of a later manifest copy that the output lacks. Throws std::bad_alloc.
 * @param later A later mod's manifest copy.
 * @param output Merged manifest so far; grows in place.
 * @return ok, mismatch when the tag, root or keys differ, or why a group cannot be added.
 */
[[nodiscard]] Status union_manifest(std::span<const std::byte> later,
                                    std::vector<std::byte>& output);

/**
 * Plans every mod's shift for every target, from the sidecars only.
 * @param entries Entries in the merge; an input with a null sidecar adds nothing.
 * @param shifts Receives the shifts.
 * @param failedTable Receives the tag of the table that failed.
 * @return ok, or why the row layer must be off.
 */
[[nodiscard]] Status
plan_shifts(std::span<const EntryInputs> entries, Shifts& shifts, std::uint32_t& failedTable);

} // namespace sunrise::middleware::content::packages::table_merge::internal
