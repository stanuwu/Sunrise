#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

#include "../../../crypto/sha256.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::table_merge {
namespace {

using internal::entry_of;
using internal::Frame;
using internal::kRowAlignment;
using internal::kWordSize;
using internal::Shifts;
using internal::Span;
using internal::tag_of;

/** Marks a stock byte no input has written yet. */
constexpr std::int32_t kNoOwner = -1;

/** @return True when the input declares an append op in this entry. */
[[nodiscard]] bool appends(const Entry* spec) noexcept {
    return spec != nullptr
           && std::any_of(spec->ops.begin(), spec->ops.end(), [](const OpRecord& op) {
                  return op.kind == OpKind::append;
              });
}

/** @return True when the input added rows to any append op of this entry. */
[[nodiscard]] bool adds_rows(const Entry* spec) noexcept {
    return spec != nullptr
           && std::any_of(spec->ops.begin(), spec->ops.end(), [](const OpRecord& op) {
                  return op.kind == OpKind::append && op.modCount > op.stockCount;
              });
}

/** @return True when any input of the entry declares an append op. */
[[nodiscard]] bool row_table(const EntryInputs& entry) noexcept {
    return std::any_of(entry.inputs.begin(), entry.inputs.end(), [&](const Input& input) {
        return appends(entry_of(input, entry.entryIndex));
    });
}

/** @return True when the op's array and every parent row exist in the stock bytes. */
[[nodiscard]] bool stock_anchored(const Entry& spec, const OpRecord& op) noexcept {
    // A parent op comes before its children, so the walk ends.
    for (const OpRecord* at = &op; at->parentOp != kTopLevelOp; at = &spec.ops[at->parentOp]) {
        if (at->parentRow >= spec.ops[at->parentOp].stockCount) {
            return false;
        }
    }
    return true;
}

/**
 * Compares the descriptor path, since op indices differ between inputs.
 * @return True when two stock-anchored ops of two inputs name the same array.
 */
[[nodiscard]] bool
same_array(const Entry& first, const OpRecord& a, const Entry& second, const OpRecord& b) noexcept {
    const OpRecord* x = &a;
    const OpRecord* y = &b;
    while (x->descriptorOffset == y->descriptorOffset) {
        const bool topX = x->parentOp == kTopLevelOp;
        const bool topY = y->parentOp == kTopLevelOp;
        if (topX || topY) {
            return topX && topY;
        }
        if (x->parentRow != y->parentRow) {
            return false;
        }
        x = &first.ops[x->parentOp];
        y = &second.ops[y->parentOp];
    }
    return false;
}

/**
 * Rows earlier inputs added to each stock-anchored op of input `index`.
 * @return One shift per op of that input; 0 for an op inside an added row.
 */
[[nodiscard]] std::vector<std::uint32_t> op_local_shifts(const EntryInputs& entry,
                                                         std::size_t index) {
    const Entry* spec = entry_of(entry.inputs[index], entry.entryIndex);
    std::vector<std::uint32_t> shifts(spec->ops.size(), 0);
    for (std::size_t op = 0; op < spec->ops.size(); ++op) {
        // An added row's number names a different row in each mod.
        if (!stock_anchored(*spec, spec->ops[op])) {
            continue;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const Entry* before = entry_of(entry.inputs[earlier], entry.entryIndex);
            if (before == nullptr) {
                continue;
            }
            for (const OpRecord& other : before->ops) {
                if (other.kind == OpKind::append && stock_anchored(*before, other)
                    && same_array(*before, other, *spec, spec->ops[op])) {
                    shifts[op] += other.modCount - other.stockCount;
                }
            }
        }
    }
    return shifts;
}

/**
 * Copies one input and shifts its sites; with the row layer off the copy stays raw.
 * @return ok, or limit when a shifted value does not fit its site.
 */
[[nodiscard]] Status shifted_copy(const EntryInputs& entry,
                                  std::size_t index,
                                  const Shifts& shifts,
                                  bool rowsOn,
                                  std::vector<std::byte>& copy) {
    const Input& input = entry.inputs[index];
    copy.assign(input.bytes.begin(), input.bytes.end());
    const Entry* spec = entry_of(input, entry.entryIndex);
    if (!rowsOn || spec == nullptr || spec->sites.empty()) {
        return Status::ok;
    }
    const std::vector<std::uint32_t> local = op_local_shifts(entry, index);
    return internal::shift_sites(input, entry.entryIndex, shifts, local, copy);
}

/**
 * Writes the stock bytes back over every diff and checks them against the stock digest.
 * @return ok, or stock when the copy was not built from the stock bytes the others name.
 */
[[nodiscard]] Status restore_stock(const Input& input,
                                   const Entry& spec,
                                   const EntryHeader& first,
                                   std::vector<std::byte>& stock) {
    const EntryHeader& header = spec.header;
    if (header.stockSize != first.stockSize || header.stockSha256 != first.stockSha256
        || header.builtSize != input.bytes.size() || header.builtSize < header.stockSize) {
        return Status::stock;
    }
    stock.assign(input.bytes.begin(), input.bytes.begin() + header.stockSize);
    for (const Diff& diff : spec.diffs) {
        std::copy(diff.old.begin(), diff.old.end(), stock.begin() + diff.offset);
    }
    crypto::sha256::Digest digest{};
    if (!crypto::sha256::hash(stock, digest)) {
        return Status::stock;
    }
    return std::equal(digest.begin(), digest.end(), header.stockSha256.begin()) ? Status::ok
                                                                                : Status::stock;
}

/** How one edit range sits against the spans the row engine owns. */
enum class Placement : std::uint8_t {
    outside,
    inside,
    crossing,
};

/** @return Where [begin, end) sits against the spans. */
[[nodiscard]] Placement
place(std::span<const Span> spans, std::uint64_t begin, std::uint64_t end) noexcept {
    bool overlaps = false;
    for (const Span& span : spans) {
        if (begin >= span.begin && end <= span.end) {
            return Placement::inside;
        }
        overlaps = overlaps || (begin < span.end && end > span.begin);
    }
    return overlaps ? Placement::crossing : Placement::outside;
}

/** The stock-region ranges one later input changes: its diffs and its sites there. */
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> edit_ranges(const Entry& spec) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    ranges.reserve(spec.diffs.size() + spec.sites.size());
    for (const Diff& diff : spec.diffs) {
        ranges.emplace_back(diff.offset, diff.offset + static_cast<std::uint32_t>(diff.old.size()));
    }
    for (const SiteRecord& site : spec.sites) {
        if (site.offset < spec.header.stockSize) {
            ranges.emplace_back(site.offset, site.offset + site.width);
        }
    }
    std::sort(ranges.begin(), ranges.end());
    return ranges;
}

/** Shared state of one entry's byte merge. */
struct ByteMerge final {
    std::span<const std::byte> stock;
    std::vector<std::byte>& output;
    /** Input that last wrote each stock byte, or kNoOwner. */
    std::vector<std::int32_t> owner;
    /** Conflicting bytes per earlier input, for the current later input. */
    std::vector<std::uint32_t> clashes;
};

/** @return The free stock range between spans that holds [begin, end), which lies outside them. */
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> gap_of(std::span<const Span> spans,
                                                             std::uint32_t begin,
                                                             std::uint32_t end,
                                                             std::uint32_t stockSize) noexcept {
    std::uint32_t low = 0;
    std::uint32_t high = stockSize;
    for (const Span& span : spans) {
        if (span.end <= begin) {
            low = (std::max)(low, span.end);
        } else if (span.begin >= end) {
            high = (std::min)(high, span.begin);
        }
    }
    return {low, high};
}

/**
 * Three-way merge of one range, one aligned word at a time, so no field mixes two mods.
 * A word where the later copy undoes an earlier mod's byte is taken whole from the later copy.
 */
void merge_range(ByteMerge& merge,
                 std::span<const std::byte> copy,
                 std::size_t later,
                 std::pair<std::uint32_t, std::uint32_t> range,
                 std::pair<std::uint32_t, std::uint32_t> gap) noexcept {
    const auto owner = static_cast<std::int32_t>(later);
    const auto undone = [&](std::uint32_t at) {
        return merge.owner[at] != kNoOwner && merge.owner[at] < owner
               && merge.output[at] != copy[at];
    };
    for (std::uint32_t word = range.first - range.first % kWordSize; word < range.second;
         word += kWordSize) {
        const std::uint32_t first = (std::max)(word, gap.first);
        const std::uint32_t last = (std::min)(word + kWordSize, gap.second);
        bool lost = false;
        for (std::uint32_t at = first; at < last && !lost; ++at) {
            lost = undone(at);
        }
        for (std::uint32_t at = first; at < last; ++at) {
            const std::byte next = copy[at];
            const bool inRange = at >= range.first && at < range.second;
            if (next == merge.output[at] || (!lost && (!inRange || next == merge.stock[at]))) {
                continue;
            }
            if (merge.owner[at] != kNoOwner && merge.owner[at] < owner) {
                ++merge.clashes[static_cast<std::size_t>(merge.owner[at])];
            }
            merge.output[at] = next;
            merge.owner[at] = owner;
        }
    }
}

/**
 * Applies every later input's stock-region edits that lie outside the row engine's spans.
 * @return ok, or format when an edit crosses a span edge.
 */
[[nodiscard]] Status merge_bytes(const EntryInputs& entry,
                                 std::span<const Entry* const> specs,
                                 std::span<const std::vector<std::byte>> copies,
                                 std::span<const Span> spans,
                                 ByteMerge& merge,
                                 std::vector<Conflict>& conflicts) {
    for (std::size_t at = 0; at < merge.stock.size(); ++at) {
        if (merge.output[at] != merge.stock[at]) {
            merge.owner[at] = 0;
        }
    }
    for (std::size_t later = 1; later < specs.size(); ++later) {
        std::fill(merge.clashes.begin(), merge.clashes.end(), 0U);
        for (const auto& [begin, end] : edit_ranges(*specs[later])) {
            const Placement where = place(spans, begin, end);
            // A site that runs past the stock end has no stock bytes to merge against.
            if (where == Placement::crossing || end > merge.stock.size()) {
                return Status::format;
            }
            if (where == Placement::outside) {
                const auto stockSize = static_cast<std::uint32_t>(merge.stock.size());
                merge_range(merge,
                            copies[later],
                            later,
                            {begin, end},
                            gap_of(spans, begin, end, stockSize));
            }
        }
        for (std::size_t earlier = 0; earlier < later; ++earlier) {
            if (merge.clashes[earlier] != 0) {
                conflicts.push_back({entry.packageId,
                                     entry.entryIndex,
                                     entry.inputs[earlier].mod,
                                     entry.inputs[later].mod,
                                     merge.clashes[earlier]});
            }
        }
    }
    return Status::ok;
}

/**
 * Appends each later input's added region at the output end, keeping its 16-byte phase.
 * @return Per input, the new start of its added region minus stockSize; 0 for input 0.
 */
[[nodiscard]] std::vector<std::int64_t> place_blobs(std::span<const std::vector<std::byte>> copies,
                                                    std::uint32_t stockSize,
                                                    std::vector<std::byte>& output) {
    std::vector<std::int64_t> delta(copies.size(), 0);
    for (std::size_t index = 1; index < copies.size(); ++index) {
        const std::uint64_t size = output.size();
        // Added regions keep their offset modulo the row alignment, so their alignment holds.
        const std::uint64_t phase = stockSize % kRowAlignment;
        std::uint64_t start = size - size % kRowAlignment + phase;
        if (start < size) {
            start += kRowAlignment;
        }
        output.resize(static_cast<std::size_t>(start));
        output.insert(output.end(), copies[index].begin() + stockSize, copies[index].end());
        delta[index] = static_cast<std::int64_t>(start) - stockSize;
    }
    return delta;
}

/**
 * Merges one table entry: proof, added regions, byte edits, rows, size word.
 * @return ok, or why the entry cannot merge.
 */
[[nodiscard]] Status merge_table(const EntryInputs& entry,
                                 std::span<const Entry* const> specs,
                                 std::span<const std::vector<std::byte>> copies,
                                 std::vector<std::byte>& output,
                                 std::vector<Conflict>& conflicts) {
    std::vector<std::byte> stock;
    std::vector<std::byte> restored;
    for (std::size_t index = 0; index < specs.size(); ++index) {
        const Status status = restore_stock(
            entry.inputs[index], *specs[index], specs[0]->header, index == 0 ? stock : restored);
        if (status != Status::ok) {
            return status;
        }
    }
    const std::uint32_t stockSize = specs[0]->header.stockSize;
    output = copies[0];
    Frame frame{stock, &output, place_blobs(copies, stockSize, output)};
    ByteMerge merge{stock,
                    output,
                    std::vector<std::int32_t>(stockSize, kNoOwner),
                    std::vector<std::uint32_t>(specs.size(), 0)};
    // The row engine unifies the inputs' ops by array; the byte merge works around its spans.
    const internal::SpanMerge bytes = [&](std::span<const Span> spans) {
        return merge_bytes(entry, specs, copies, spans, merge, conflicts);
    };
    const std::size_t first = conflicts.size();
    const Status status =
        internal::merge_ops(frame, copies, entry.inputs, entry.entryIndex, bytes, conflicts);
    for (std::size_t index = first; index < conflicts.size(); ++index) {
        conflicts[index].packageId = entry.packageId;
        conflicts[index].entryIndex = entry.entryIndex;
    }
    if (status == Status::ok) {
        reader::layout::carry_size_word(stock, output);
    }
    return status;
}

/**
 * Merges one entry that two or more inputs change.
 * @return ok with the merged bytes, or why the entry falls back.
 */
[[nodiscard]] Status merge_entry(const EntryInputs& entry,
                                 std::span<const std::vector<std::byte>> copies,
                                 std::vector<std::byte>& output,
                                 std::vector<Conflict>& conflicts) {
    std::vector<const Entry*> specs;
    for (const Input& input : entry.inputs) {
        const Entry* spec = entry_of(input, entry.entryIndex);
        if (spec == nullptr) {
            return Status::unsupported;
        }
        if (spec->header.kind != entry_of(entry.inputs[0], entry.entryIndex)->header.kind) {
            return Status::mismatch;
        }
        specs.push_back(spec);
    }
    switch (specs[0]->header.kind) {
    case EntryKind::table:
        return merge_table(entry, specs, copies, output, conflicts);
    case EntryKind::manifest:
        output = copies[0];
        for (std::size_t index = 1; index < copies.size(); ++index) {
            const Status status = internal::union_manifest(copies[index], output);
            if (status != Status::ok) {
                return status;
            }
        }
        return Status::ok;
    case EntryKind::opaque:
    case EntryKind::added:
        break;
    }
    return Status::unsupported;
}

/** Why one pass turned the row layer off. */
struct RowsOff final {
    Status reason{Status::ok};
    std::uint32_t table{};
};

/** One merge pass over every entry, with fixed shifts. */
class Pass final {
public:
    Pass(const Shifts& shifts, bool rowsOn, Outcome& outcome) noexcept
        : shifts_(shifts), rowsOn_(rowsOn), outcome_(outcome) {}

    /** @return ok, or why the row layer must turn off and the pass rerun. */
    [[nodiscard]] RowsOff run(std::span<const EntryInputs> entries) {
        for (const EntryInputs& entry : entries) {
            if (entry.inputs.empty()) {
                continue;
            }
            const RowsOff off = entry.inputs.size() == 1 ? single(entry) : shared(entry);
            if (off.reason != Status::ok) {
                return off;
            }
        }
        return {};
    }

private:
    /** An entry one mod changes is replaced only when its sites moved. */
    [[nodiscard]] RowsOff single(const EntryInputs& entry) {
        std::vector<std::byte> copy;
        const Status status = shifted_copy(entry, 0, shifts_, rowsOn_, copy);
        if (status != Status::ok) {
            return {status, tag_of(entry.packageId, entry.entryIndex)};
        }
        replace_if_changed(entry, std::move(copy), entry.inputs[0].bytes);
        return {};
    }

    /** @return ok, or why the row layer must turn off. */
    [[nodiscard]] RowsOff shared(const EntryInputs& entry) {
        const bool rows = row_table(entry);
        const std::uint32_t tag = tag_of(entry.packageId, entry.entryIndex);
        if (rows && !rowsOn_) {
            drop(entry, tag);
            return {};
        }
        std::vector<std::vector<std::byte>> copies(entry.inputs.size());
        for (std::size_t index = 0; index < copies.size(); ++index) {
            const Status status = shifted_copy(entry, index, shifts_, rowsOn_, copies[index]);
            if (status != Status::ok) {
                return {status, tag};
            }
        }
        std::vector<std::byte> output;
        std::vector<Conflict> conflicts;
        const Status status = merge_entry(entry, copies, output, conflicts);
        if (status == Status::ok) {
            outcome_.conflicts.insert(outcome_.conflicts.end(), conflicts.begin(), conflicts.end());
            replace_if_changed(entry, std::move(output), entry.inputs.back().bytes);
            return {};
        }
        if (rows) {
            return {status, tag};
        }
        outcome_.fallbacks.push_back({entry.packageId, entry.entryIndex, status});
        // The last input wins, with its sites still pointing at its shifted rows.
        replace_if_changed(entry, std::move(copies.back()), entry.inputs.back().bytes);
        return {};
    }

    /** A row table with the row layer off keeps the last mod's rows only. */
    void drop(const EntryInputs& entry, std::uint32_t tag) {
        outcome_.fallbacks.push_back({entry.packageId, entry.entryIndex, outcome_.rows});
        for (std::size_t index = 0; index + 1 < entry.inputs.size(); ++index) {
            if (adds_rows(entry_of(entry.inputs[index], entry.entryIndex))) {
                outcome_.dropped.push_back({entry.inputs[index].mod, tag});
            }
        }
    }

    void replace_if_changed(const EntryInputs& entry,
                            std::vector<std::byte> bytes,
                            std::span<const std::byte> served) {
        if (!std::equal(bytes.begin(), bytes.end(), served.begin(), served.end())) {
            outcome_.replacements.push_back({entry.packageId, entry.entryIndex, std::move(bytes)});
        }
    }

    const Shifts& shifts_;
    bool rowsOn_;
    Outcome& outcome_;
};

/**
 * Step a: a sidecar whose entry size or sites do not hold is ignored, and so is all merge data
 * of its mod. Its rows may have to move, and an ignored entry could not follow them.
 * @return The entries with those mods' sidecars removed.
 */
[[nodiscard]] std::vector<EntryInputs> check_inputs(std::span<const EntryInputs> entries,
                                                    Outcome& outcome) {
    std::vector<EntryInputs> work(entries.begin(), entries.end());
    std::set<std::pair<std::size_t, std::uint16_t>> seen;
    std::set<std::size_t> ignoredMods;
    for (const EntryInputs& entry : work) {
        for (const Input& input : entry.inputs) {
            const Entry* spec = entry_of(input, entry.entryIndex);
            if (spec == nullptr) {
                continue;
            }
            // Site offsets were checked against builtSize, so the size must hold first.
            const Status status = spec->header.builtSize != input.bytes.size()
                                      ? Status::binding
                                      : internal::check_sites(input, entry.entryIndex);
            if (status == Status::ok) {
                continue;
            }
            ignoredMods.insert(input.mod);
            // One warning per mod and package.
            if (seen.insert({input.mod, entry.packageId}).second) {
                outcome.ignored.push_back({input.mod, entry.packageId, status});
            }
        }
    }
    for (EntryInputs& entry : work) {
        for (Input& input : entry.inputs) {
            if (ignoredMods.contains(input.mod)) {
                input.sidecar = nullptr;
            }
        }
    }
    return work;
}

/** Runs the plan and the passes; throws std::bad_alloc. */
[[nodiscard]] Outcome merge_all(std::span<const EntryInputs> entries) {
    Outcome outcome;
    const std::vector<EntryInputs> work = check_inputs(entries, outcome);
    Shifts shifts;
    outcome.rows = internal::plan_shifts(work, shifts, outcome.rowsTable);
    if (outcome.rows != Status::ok) {
        shifts.clear();
    } else {
        outcome.rowsTable = 0;
    }
    const RowsOff off = Pass(shifts, outcome.rows == Status::ok, outcome).run(work);
    if (off.reason != Status::ok) {
        // Row tables cannot merge, so every shift is dropped and the pass reruns.
        outcome.rows = off.reason;
        outcome.rowsTable = off.table;
        outcome.replacements.clear();
        outcome.fallbacks.clear();
        outcome.conflicts.clear();
        outcome.dropped.clear();
        shifts.clear();
        // With the row layer off no entry can turn it off again.
        (void)Pass(shifts, false, outcome).run(work);
    }
    std::sort(outcome.replacements.begin(),
              outcome.replacements.end(),
              [](const Replacement& a, const Replacement& b) {
                  return std::tie(a.packageId, a.entryIndex) < std::tie(b.packageId, b.entryIndex);
              });
    return outcome;
}

} // namespace

/** Any allocation failure serves every shared entry later-wins. */
Outcome merge(std::span<const EntryInputs> entries) noexcept {
    try {
        return merge_all(entries);
    } catch (...) {
        // Container growth throws bad_alloc or length_error; both mean out of capacity.
        Outcome outcome;
        outcome.rows = Status::capacity;
        try {
            for (const EntryInputs& entry : entries) {
                if (entry.inputs.size() > 1) {
                    outcome.fallbacks.push_back(
                        {entry.packageId, entry.entryIndex, Status::capacity});
                }
            }
        } catch (...) {
            outcome.fallbacks.clear();
        }
        return outcome;
    }
}

/** @return The status as one lowercase log token. */
const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::format:
        return "format";
    case Status::version:
        return "version";
    case Status::binding:
        return "binding";
    case Status::stock:
        return "stock";
    case Status::sites:
        return "sites";
    case Status::mismatch:
        return "mismatch";
    case Status::pointer:
        return "pointer";
    case Status::order:
        return "order";
    case Status::duplicate:
        return "duplicate";
    case Status::pairs:
        return "pairs";
    case Status::limit:
        return "limit";
    case Status::capacity:
        return "capacity";
    case Status::unsupported:
        return "unsupported";
    }
    return "unknown";
}

/** @return A short readable reason for a mod warning. */
const char* status_text(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::format:
        return "the merge data is malformed";
    case Status::version:
        return "the merge data has an unknown version";
    case Status::binding:
        return "the merge data was built for another package file";
    case Status::stock:
        return "the mods were built against different game files";
    case Status::sites:
        return "a row reference names a row the mod did not add";
    case Status::mismatch:
        return "the mods describe the table differently";
    case Status::pointer:
        return "a row pointer does not land on a row";
    case Status::order:
        return "the merged keys are out of order";
    case Status::duplicate:
        return "two mods add the same key";
    case Status::pairs:
        return "paired tables gained different row counts";
    case Status::limit:
        return "the merged table holds too many rows";
    case Status::capacity:
        return "the merge ran out of memory";
    case Status::unsupported:
        return "this entry cannot be merged";
    }
    return "unknown";
}

} // namespace sunrise::middleware::content::packages::table_merge
