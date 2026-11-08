#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "../tables/definition_index_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::table_merge::internal {
namespace {

using Bytes = std::span<const std::byte>;

/** Rows start right after a typed array header. */
using Header = reader::layout::TypedArrayHeader;

/** Source id of the stock bytes, beside the input indices. */
constexpr std::size_t kStock = (std::numeric_limits<std::size_t>::max)();
/** Merged id of no op: the parent of a top-level op, or an op an input does not declare. */
constexpr std::uint32_t kNoOp = (std::numeric_limits<std::uint32_t>::max)();
/** Sidecar offsets are u32, so a merged entry must stay below 4 GiB. */
constexpr std::uint64_t kEntryLimit = (std::numeric_limits<std::uint32_t>::max)();
/** Row skip marks: the byte merge leaves both alone; a nested descriptor is rebuilt. */
constexpr std::uint8_t kPointerByte = 1;
constexpr std::uint8_t kNestedByte = 2;

/** Bytes two inputs both changed, keyed by (earlier input, later input). */
using Tally = std::map<std::pair<std::size_t, std::size_t>, std::uint32_t>;

/** @return True when [at, at + size) lies inside `bytes`. */
[[nodiscard]] bool fits(Bytes bytes, std::uint64_t at, std::uint64_t size) noexcept {
    return at <= bytes.size() && size <= bytes.size() - at;
}

/**
 * Three-way merges one input's row into merged, one row-relative word at a time.
 * A word the input changed is taken whole, minus skipped bytes; a clash goes to the input.
 */
void three_way(std::span<std::byte> merged,
               std::span<std::size_t> writers,
               Bytes stock,
               Bytes view,
               std::span<const std::uint8_t> skip,
               std::size_t input,
               Tally& tally) {
    const auto open = [&](std::size_t at) { return skip.empty() || skip[at] == 0; };
    for (std::size_t word = 0; word < merged.size(); word += kWordSize) {
        const std::size_t end = (std::min)(word + kWordSize, merged.size());
        bool changed = false;
        for (std::size_t at = word; at < end; ++at) {
            changed = changed || (open(at) && view[at] != stock[at]);
        }
        for (std::size_t at = word; changed && at < end; ++at) {
            if (!open(at) || merged[at] == view[at]) {
                continue;
            }
            if (merged[at] != stock[at]) {
                ++tally[{writers[at], input}];
            }
            merged[at] = view[at];
            writers[at] = input;
        }
    }
}

/** One array as one source holds it. header is 0 when the source has none. */
struct View final {
    std::size_t descriptor{};
    std::size_t header{};
    std::size_t data{};
    std::uint64_t count{};
};

/** One op merged across every input that declares it. */
struct Op final {
    std::uint32_t parent{kNoOp};
    OpRecord record{};
    /** First input that declares the op; its shape is the reference. */
    std::size_t owner{};
    std::uint32_t stride{};
    /** Per input: its op index, or kNoOp. */
    std::vector<std::uint32_t> local;
    View stock;
    std::vector<View> views;
    /** Per input: rows that earlier inputs added. */
    std::vector<std::uint64_t> before;
    std::uint64_t count{};
    /** Merged header in the output, or 0 when the merged op has no array. */
    std::size_t header{};
    /** 1 for each row byte a pointer field covers. */
    std::vector<std::uint8_t> pointerBytes;
    /** Sorted map only: the merged rows in key order. */
    std::vector<std::byte> mapRows;
};

/** One pointer field: where it was read and its target mapped into the output. */
struct Pointer final {
    bool null{true};
    std::size_t target{};
    std::size_t raw{};
    std::uint64_t count{};
    std::size_t source{kStock};
    std::size_t field{};
};

/** @return True when two pointer fields name the same output target. */
[[nodiscard]] bool same(const Pointer& first, const Pointer& second) noexcept {
    return first.null == second.null && first.count == second.count
           && (first.null || first.target == second.target);
}

/** One array span of one source, for the location map. */
struct Region final {
    std::size_t begin{};
    std::size_t end{};
    std::uint32_t op{};
    std::size_t source{};
};

/** Where one address landed in the merged ops. op is kNoOp outside every array. */
struct Location final {
    std::uint32_t op{kNoOp};
    bool inRow{};
    std::size_t offset{};
};

/** One sorted map row while the union is built. */
struct MapSlot final {
    std::vector<std::byte> row;
    /** The stock row, empty for a key some input added. */
    Bytes stock;
    /** Per byte: the input that last wrote it. Empty until a stock row changes. */
    std::vector<std::size_t> writers;
    std::size_t adder{kStock};
};

/** @return The region holding `at`, or null. Regions are sorted and do not overlap. */
[[nodiscard]] const Region* find_region(const std::vector<Region>& regions,
                                        std::size_t at) noexcept {
    auto found = std::upper_bound(
        regions.begin(), regions.end(), at, [](std::size_t value, const Region& region) {
            return value < region.begin;
        });
    if (found == regions.begin()) {
        return nullptr;
    }
    --found;
    return at < found->end ? &*found : nullptr;
}

/** Sorts one source's regions. @return False when two of them overlap. */
[[nodiscard]] bool sort_regions(std::vector<Region>& regions) {
    std::sort(regions.begin(), regions.end(), [](const Region& first, const Region& second) {
        return first.begin < second.begin;
    });
    const auto overlap = [](const Region& first, const Region& second) {
        return first.end > second.begin;
    };
    return std::adjacent_find(regions.begin(), regions.end(), overlap) == regions.end();
}

/** Merges the ops of one entry: unify, resolve, count, place, write. */
class Engine final {
public:
    Engine(Bytes stock,
           std::span<const Input> inputs,
           std::uint32_t entryIndex,
           std::span<const std::vector<std::byte>> copies) noexcept
        : stock_(stock), inputs_(inputs), entryIndex_(entryIndex), copies_(copies) {}

    [[nodiscard]] Status
    merge(Frame& frame, const SpanMerge& byteMerge, std::vector<Conflict>& conflicts);

private:
    Status build();
    void spans(std::vector<Span>& output) const;
    Status unify(std::size_t input, std::uint32_t local);
    Status admit(const OpRecord& record, const Shape& shape, std::uint32_t parent);
    Status agree(const Op& op, const OpRecord& record, const Shape& shape) const;
    Status resolve(std::uint32_t id, std::size_t source, View& output) const;
    Status count_rows();
    Status map_rows(const Op& op, std::size_t source, std::map<std::uint32_t, MapSlot>& slots);
    Status place();
    Status index_regions();
    Status map(std::size_t source, std::size_t at, std::size_t& mapped, Location& where) const;
    Status read_pointer(std::size_t source,
                        std::size_t field,
                        FieldKind kind,
                        std::uint32_t rowOp,
                        Pointer& output) const;
    void write_pointer(std::size_t at, FieldKind kind, const Pointer& pointer) const;
    void write_descriptor(std::size_t at, const Op& op) const;
    Status write_op(std::uint32_t id);
    Status write_stock_row(std::uint32_t id, std::uint64_t row);
    Status pick_pointer(const Op& op, std::uint64_t row, std::size_t index, Pointer& pick);
    Status merge_pointers(const Op& op, std::uint64_t row, std::size_t at);
    Status emit(std::size_t at,
                const ShapeField& field,
                std::size_t holder,
                const Pointer& pointer,
                bool write = true);
    Status write_added_rows(std::uint32_t id, std::size_t input);
    Status walk(std::size_t source, std::size_t holder, std::size_t field, std::uint16_t shape);
    Status
    walk_object(std::size_t source, std::size_t holder, std::size_t object, std::uint16_t shape);
    Status
    relocate(std::size_t source, std::size_t holder, std::size_t at, const ShapeField& field);
    Status check_keys(const Op& op) const;

    Bytes source_bytes(std::size_t source) const noexcept {
        return source == kStock ? stock_ : Bytes(copies_[source]);
    }
    static const View& view(const Op& op, std::size_t source) noexcept {
        return source == kStock ? op.stock : op.views[source];
    }
    /** @return The input whose shapes describe `source`'s rows of `op`. */
    static std::size_t holder_of(const Op& op, std::size_t source) noexcept {
        return source != kStock && op.local[source] != kNoOp ? source : op.owner;
    }
    /** @return The shape `input` declares for `op`. The input declares it. */
    const Shape& shape_of(const Op& op, std::size_t input) const noexcept {
        return *inputs_[input].sidecar->shape(records_[input][op.local[input]].shape);
    }
    /** @return The merged op a row pointer field of `input`'s shape names. */
    std::uint32_t row_op(std::size_t input, const ShapeField& field) const noexcept {
        if (field.kind != FieldKind::rowPointer || field.ref >= merged_[input].size()) {
            return kNoOp;
        }
        return merged_[input][field.ref];
    }

    Bytes stock_;
    std::span<const Input> inputs_;
    std::uint32_t entryIndex_{};
    std::span<const std::vector<std::byte>> copies_;
    std::vector<Op> ops_;
    /** Per input: the ops it declares. */
    std::vector<std::span<const OpRecord>> records_;
    /** Per input: its op index to merged op id. */
    std::vector<std::vector<std::uint32_t>> merged_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> ids_;
    /** Nested ops by (parent op, parent row). */
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::vector<std::uint32_t>> children_;
    Frame* frame_{};
    std::vector<Region> stockRegions_;
    std::vector<std::vector<Region>> inputRegions_;
    Tally tally_;
    std::set<std::pair<std::size_t, std::size_t>> visited_;
    std::vector<std::byte> row_;
    std::vector<std::size_t> writers_;
    std::vector<std::uint8_t> rowSkip_;
    std::vector<std::uint8_t> changed_;
};

/** Unifies every input's ops by (parent, parent row, descriptor) and reads their arrays. */
Status Engine::build() {
    merged_.resize(inputs_.size());
    for (const Input& input : inputs_) {
        const Entry* entry = entry_of(input, entryIndex_);
        records_.emplace_back();
        if (entry != nullptr) {
            records_.back() = entry->ops;
        }
    }
    Status status = Status::ok;
    for (std::size_t input = 0; status == Status::ok && input < inputs_.size(); ++input) {
        const auto records = records_[input];
        merged_[input].assign(records.size(), kNoOp);
        // A parent op comes before its children, so its merged id is already known.
        for (std::uint32_t local = 0; status == Status::ok && local < records.size(); ++local) {
            status = unify(input, local);
        }
    }
    for (std::uint32_t id = 0; status == Status::ok && id < ops_.size(); ++id) {
        Op& op = ops_[id];
        status = resolve(id, kStock, op.stock);
        if (status == Status::ok && op.stock.count != op.record.stockCount) {
            status = Status::stock;
        }
        // An input that does not declare the op must keep the stock row count.
        for (std::size_t input = 0; status == Status::ok && input < copies_.size(); ++input) {
            status = resolve(id, input, op.views[input]);
            const std::uint64_t expected = op.local[input] == kNoOp
                                               ? op.record.stockCount
                                               : records_[input][op.local[input]].modCount;
            if (status == Status::ok && op.views[input].count != expected) {
                status = Status::mismatch;
            }
        }
    }
    return status;
}

/** Maps op `local` of one input onto a merged op, creating it on first sight. */
Status Engine::unify(std::size_t input, std::uint32_t local) {
    const OpRecord& record = records_[input][local];
    const Shape& shape = *inputs_[input].sidecar->shape(record.shape);
    const bool nested = record.parentOp != kTopLevelOp;
    const std::uint32_t parent = nested ? merged_[input][record.parentOp] : kNoOp;
    // An array inside an added row belongs to that row; the row's own pointers move it.
    if (nested && (parent == kNoOp || record.parentRow >= ops_[parent].record.stockCount)) {
        return Status::ok;
    }
    const auto key =
        std::make_tuple(parent, parent == kNoOp ? 0U : record.parentRow, record.descriptorOffset);
    const auto [found, created] = ids_.try_emplace(key, static_cast<std::uint32_t>(ops_.size()));
    const Status status =
        created ? admit(record, shape, parent) : agree(ops_[found->second], record, shape);
    if (status != Status::ok) {
        return status;
    }
    if (created) {
        std::vector<std::uint8_t> pointerBytes(shape.stride, 0);
        for (const ShapeField& field : shape.fields) {
            std::fill_n(
                pointerBytes.begin() + field.rowOffset, field_size(field.kind), kPointerByte);
        }
        const std::size_t count = inputs_.size();
        ops_.push_back({.parent = parent,
                        .record = record,
                        .owner = input,
                        .stride = shape.stride,
                        .local = std::vector<std::uint32_t>(count, kNoOp),
                        .views = std::vector<View>(count),
                        .before = std::vector<std::uint64_t>(count),
                        .pointerBytes = std::move(pointerBytes)});
    }
    ops_[found->second].local[input] = local;
    merged_[input][local] = found->second;
    return Status::ok;
}

/** @return ok when the engine can rebuild a new op of this form, else unsupported. */
Status Engine::admit(const OpRecord& record, const Shape& shape, std::uint32_t parent) {
    // Sorted map rows move by key, so they may hold no pointers.
    if (record.kind == OpKind::sortedMap && !shape.fields.empty()) {
        return Status::unsupported;
    }
    if (parent == kNoOp) {
        return Status::ok;
    }
    // A nested op lives in a row of an append op.
    if (ops_[parent].record.kind != OpKind::append) {
        return Status::unsupported;
    }
    children_[{parent, record.parentRow}].push_back(static_cast<std::uint32_t>(ops_.size()));
    return Status::ok;
}

/** @return ok when a later input declares the op the same way, else mismatch. */
Status Engine::agree(const Op& op, const OpRecord& record, const Shape& shape) const {
    const OpRecord& first = op.record;
    const Shape& reference = shape_of(op, op.owner);
    const auto sameField = [](const ShapeField& one, const ShapeField& other) {
        return one.rowOffset == other.rowOffset && one.kind == other.kind;
    };
    if (record.kind != first.kind || record.flags != first.flags
        || record.keyWidth != first.keyWidth || record.keyOffset != first.keyOffset
        || record.stockCount != first.stockCount || record.limit != first.limit
        || shape.stride != reference.stride
        || !std::equal(shape.fields.begin(),
                       shape.fields.end(),
                       reference.fields.begin(),
                       reference.fields.end(),
                       sameField)) {
        return Status::mismatch;
    }
    return Status::ok;
}

/** Reads op `id`'s array as `source` holds it. @return ok, or format when it does not fit. */
Status Engine::resolve(std::uint32_t id, std::size_t source, View& output) const {
    const Op& op = ops_[id];
    const Bytes bytes = source_bytes(source);
    std::uint64_t at = op.record.descriptorOffset;
    if (op.parent != kNoOp) {
        // The parent row is a stock row, which every source holds.
        const Op& parent = ops_[op.parent];
        at += view(parent, source).data + std::uint64_t{op.record.parentRow} * parent.stride;
    }
    tables::Array array{};
    if (!fits(bytes, at, kDescriptorSize)
        || !tables::find_optional_array_at(bytes, static_cast<std::size_t>(at), array)) {
        return Status::format;
    }
    output.descriptor = static_cast<std::size_t>(at);
    output.data = array.dataOffset;
    output.header = array.dataOffset == 0 ? 0 : array.dataOffset - Header::kSize;
    output.count = array.count;
    return fits(bytes, output.data, array.count * op.stride) ? Status::ok : Status::format;
}

/** Counts every merged op, builds each sorted map's union, and checks the limit. */
Status Engine::count_rows() {
    for (Op& op : ops_) {
        if (op.record.kind == OpKind::sortedMap) {
            std::map<std::uint32_t, MapSlot> slots;
            Status status = map_rows(op, kStock, slots);
            for (std::size_t input = 0; status == Status::ok && input < inputs_.size(); ++input) {
                status = map_rows(op, input, slots);
            }
            if (status != Status::ok) {
                return status;
            }
            op.count = slots.size();
            for (const auto& slot : slots) {
                op.mapRows.insert(op.mapRows.end(), slot.second.row.begin(), slot.second.row.end());
            }
        } else {
            std::uint64_t added = 0;
            for (std::size_t input = 0; input < inputs_.size(); ++input) {
                op.before[input] = added;
                added += op.views[input].count - op.record.stockCount;
            }
            op.count = op.record.stockCount + added;
        }
        if (op.count > op.record.limit) {
            return Status::limit;
        }
    }
    return Status::ok;
}

/**
 * Adds one source's sorted map rows to the union.
 * @return ok, order when the source's keys do not ascend, mismatch when it dropped a stock key.
 */
Status Engine::map_rows(const Op& op, std::size_t source, std::map<std::uint32_t, MapSlot>& slots) {
    const View& from = view(op, source);
    const Bytes bytes = source_bytes(source);
    std::uint64_t present = 0;
    for (std::uint64_t row = 0; row < from.count; ++row) {
        const std::size_t at = from.data + row * op.stride;
        const auto key = load<std::uint32_t>(bytes, at + op.record.keyOffset);
        if (row != 0 && key <= load<std::uint32_t>(bytes, at - op.stride + op.record.keyOffset)) {
            return Status::order;
        }
        const Bytes rowBytes = bytes.subspan(at, op.stride);
        auto [found, created] = slots.try_emplace(key);
        MapSlot& slot = found->second;
        if (created) {
            slot.row.assign(rowBytes.begin(), rowBytes.end());
            slot.stock = source == kStock ? rowBytes : Bytes{};
            slot.adder = source;
        } else if (!slot.stock.empty()) {
            ++present;
            slot.writers.resize(op.stride, kStock);
            three_way(slot.row, slot.writers, slot.stock, rowBytes, {}, source, tally_);
        } else if (!std::equal(rowBytes.begin(), rowBytes.end(), slot.row.begin())) {
            // Two inputs added one key with different rows: the later row wins.
            for (std::size_t index = 0; index < op.stride; ++index) {
                tally_[{slot.adder, source}] += slot.row[index] != rowBytes[index] ? 1U : 0U;
            }
            slot.row.assign(rowBytes.begin(), rowBytes.end());
            slot.adder = source;
        }
    }
    return source == kStock || present == op.record.stockCount ? Status::ok : Status::mismatch;
}

/** Places every merged array at the output end and writes its header. */
Status Engine::place() {
    std::vector<std::byte>& output = *frame_->output;
    for (Op& op : ops_) {
        const auto held = std::find_if(
            op.views.begin(), op.views.end(), [](const View& from) { return from.header != 0; });
        const std::size_t source =
            op.stock.header != 0 ? kStock : static_cast<std::size_t>(held - op.views.begin());
        // No source holds a header, so no source holds a row either.
        if (source == inputs_.size()) {
            continue;
        }
        const View& from = view(op, source);
        const Bytes bytes = source_bytes(source);
        const std::uint64_t header = align(output.size() + Header::kMarkerBack, kRowAlignment);
        const std::uint64_t end = header + Header::kSize + op.count * op.stride;
        if (end > kEntryLimit) {
            return Status::capacity;
        }
        output.resize(static_cast<std::size_t>(end));
        op.header = static_cast<std::size_t>(header);
        std::memcpy(output.data() + op.header - Header::kMarkerBack,
                    bytes.data() + from.header - Header::kMarkerBack,
                    Header::kMarkerBack + Header::kSize);
        store(output, op.header + Header::kCount, op.count);
    }
    return Status::ok;
}

/** Indexes every source's array spans for the location map. */
Status Engine::index_regions() {
    inputRegions_.assign(inputs_.size(), {});
    for (std::uint32_t id = 0; id < ops_.size(); ++id) {
        const Op& op = ops_[id];
        for (std::size_t source = 0; source <= inputs_.size(); ++source) {
            const bool stock = source == inputs_.size();
            const View& from = stock ? op.stock : op.views[source];
            // An array an input left in place is found through the stock span.
            if (from.header == 0
                || (!stock && from.header == op.stock.header && from.count == op.stock.count)) {
                continue;
            }
            const Region region{from.header - Header::kMarkerBack,
                                from.data + static_cast<std::size_t>(from.count) * op.stride,
                                id,
                                stock ? kStock : source};
            (stock ? stockRegions_ : inputRegions_[source]).push_back(region);
        }
    }
    bool valid = sort_regions(stockRegions_);
    for (auto& regions : inputRegions_) {
        valid = sort_regions(regions) && valid;
    }
    return valid ? Status::ok : Status::format;
}

/** Maps one source address into the output. @return ok, or pointer when it has no place. */
Status Engine::map(std::size_t source, std::size_t at, std::size_t& mapped, Location& where) const {
    where = {};
    const Region* hit = source == kStock ? nullptr : find_region(inputRegions_[source], at);
    hit = hit == nullptr ? find_region(stockRegions_, at) : hit;
    if (hit == nullptr && at < stock_.size()) {
        mapped = at;
        return Status::ok;
    }
    if (hit == nullptr) {
        const bool added = source != kStock && at < copies_[source].size();
        mapped = added ? at + static_cast<std::size_t>(frame_->blobDelta[source]) : 0;
        return added ? Status::ok : Status::pointer;
    }
    const Op& op = ops_[hit->op];
    const View& from = view(op, hit->source);
    if (op.header == 0) {
        return Status::pointer;
    }
    where.op = hit->op;
    if (at < from.data) {
        mapped = op.header - from.header + at;
        return Status::ok;
    }
    // Sorted map rows move by key, so nothing may point into them.
    if (op.record.kind != OpKind::append) {
        return Status::pointer;
    }
    const std::uint64_t index = (at - from.data) / op.stride;
    where.inRow = true;
    where.offset = (at - from.data) % op.stride;
    // Added rows follow the stock rows and every earlier input's added rows.
    const std::uint64_t row = index < op.record.stockCount ? index : index + op.before[hit->source];
    mapped = op.header + Header::kSize + static_cast<std::size_t>(row) * op.stride + where.offset;
    return Status::ok;
}

/** Reads and maps one pointer field. @return ok, format, or pointer when it has no place. */
Status Engine::read_pointer(std::size_t source,
                            std::size_t field,
                            FieldKind kind,
                            std::uint32_t rowOp,
                            Pointer& output) const {
    const Bytes bytes = source_bytes(source);
    output = {.source = source, .field = field};
    if (!fits(bytes, field, field_size(kind))) {
        return Status::format;
    }
    std::size_t base = field;
    if (kind == FieldKind::ownedArray) {
        output.count = load<std::uint64_t>(bytes, field + reader::layout::kArrayFieldCount);
        base = field + reader::layout::kArrayFieldRelative;
    }
    const auto value = load<std::int64_t>(bytes, base);
    if (value == 0) {
        return Status::ok;
    }
    if (value < -static_cast<std::int64_t>(base)
        || value >= static_cast<std::int64_t>(bytes.size() - base)) {
        return Status::pointer;
    }
    output.null = false;
    output.raw = static_cast<std::size_t>(static_cast<std::int64_t>(base) + value);
    Location where{};
    const Status status = map(source, output.raw, output.target, where);
    // A row pointer must land on a row start of the op its shape names.
    if (status == Status::ok && kind == FieldKind::rowPointer
        && (rowOp == kNoOp || !where.inRow || where.op != rowOp || where.offset != 0)) {
        return Status::pointer;
    }
    return status;
}

/** Writes one pointer field at an output address, counted from that address. */
void Engine::write_pointer(std::size_t at, FieldKind kind, const Pointer& pointer) const {
    std::vector<std::byte>& output = *frame_->output;
    std::size_t base = at;
    if (kind == FieldKind::ownedArray) {
        store(output, at + reader::layout::kArrayFieldCount, pointer.count);
        base = at + reader::layout::kArrayFieldRelative;
    }
    const auto offset = static_cast<std::int64_t>(pointer.target) - static_cast<std::int64_t>(base);
    store(output, base, pointer.null ? std::int64_t{0} : offset);
}

/** Writes one op's descriptor at an output address. */
void Engine::write_descriptor(std::size_t at, const Op& op) const {
    const Pointer pointer{.null = op.header == 0, .target = op.header, .count = op.count};
    write_pointer(at, FieldKind::ownedArray, pointer);
}

/** Writes one pointer at `at`, then relocates what it owns. @param field Holder's field. */
Status Engine::emit(std::size_t at,
                    const ShapeField& field,
                    std::size_t holder,
                    const Pointer& pointer,
                    bool write) {
    if (write) {
        write_pointer(at, field.kind, pointer);
    }
    if (pointer.null || field.kind == FieldKind::rowPointer) {
        return Status::ok;
    }
    return field.kind == FieldKind::ownedArray
               ? walk(pointer.source, holder, pointer.field, field.ref)
               : walk_object(pointer.source, holder, pointer.raw, field.ref);
}

/** Writes one merged op: its descriptor, its rows and its key checks. */
Status Engine::write_op(std::uint32_t id) {
    const Op& op = ops_[id];
    if (op.parent == kNoOp) {
        write_descriptor(op.record.descriptorOffset, op);
    }
    if (op.record.kind == OpKind::sortedMap) {
        const auto at = frame_->output->begin() + static_cast<std::ptrdiff_t>(op.header);
        std::copy(op.mapRows.begin(), op.mapRows.end(), at + Header::kSize);
        return Status::ok;
    }
    Status status = Status::ok;
    for (std::uint64_t row = 0; status == Status::ok && row < op.record.stockCount; ++row) {
        status = write_stock_row(id, row);
    }
    for (std::size_t input = 0; status == Status::ok && input < inputs_.size(); ++input) {
        status = write_added_rows(id, input);
    }
    return status == Status::ok ? check_keys(op) : status;
}

/** Merges stock row `row` of op `id` across the inputs and writes it. */
Status Engine::write_stock_row(std::uint32_t id, std::uint64_t row) {
    const Op& op = ops_[id];
    const Bytes stockRow = stock_.subspan(op.stock.data + row * op.stride, op.stride);
    row_.assign(stockRow.begin(), stockRow.end());
    writers_.assign(op.stride, kStock);
    changed_.assign(inputs_.size(), 0);
    rowSkip_ = op.pointerBytes;
    const auto found = children_.find({id, row});
    std::span<const std::uint32_t> nested;
    if (found != children_.end()) {
        nested = found->second;
    }
    for (const std::uint32_t child : nested) {
        const auto at = rowSkip_.begin() + ops_[child].record.descriptorOffset;
        std::fill_n(at, kDescriptorSize, kNestedByte);
    }
    for (std::size_t input = 0; input < inputs_.size(); ++input) {
        const View& from = op.views[input];
        const Bytes viewRow = Bytes(copies_[input]).subspan(from.data + row * op.stride, op.stride);
        if (from.data == op.stock.data
            && std::equal(viewRow.begin(), viewRow.end(), stockRow.begin())) {
            continue;
        }
        changed_[input] = 1;
        three_way(row_, writers_, stockRow, viewRow, rowSkip_, input, tally_);
    }
    const std::size_t at = op.header + Header::kSize + row * op.stride;
    std::memcpy(frame_->output->data() + at, row_.data(), op.stride);
    const Status status = merge_pointers(op, row, at);
    for (const std::uint32_t child : nested) {
        write_descriptor(at + ops_[child].record.descriptorOffset, ops_[child]);
    }
    return status;
}

/**
 * Three-way merges field `index` of one stock row by mapped target.
 * @return ok, or pointer when two inputs moved it to different targets.
 */
Status Engine::pick_pointer(const Op& op, std::uint64_t row, std::size_t index, Pointer& pick) {
    const ShapeField& field = shape_of(op, op.owner).fields[index];
    const std::size_t offset = row * op.stride + field.rowOffset;
    Status status =
        read_pointer(kStock, op.stock.data + offset, field.kind, row_op(op.owner, field), pick);
    const Pointer stock = pick;
    for (std::size_t input = 0; status == Status::ok && input < inputs_.size(); ++input) {
        if (changed_[input] == 0) {
            continue;
        }
        const std::size_t holder = holder_of(op, input);
        const std::uint32_t rowOp = row_op(holder, shape_of(op, holder).fields[index]);
        Pointer candidate{};
        status = read_pointer(input, op.views[input].data + offset, field.kind, rowOp, candidate);
        if (status != Status::ok || same(candidate, stock) || same(candidate, pick)) {
            continue;
        }
        if (!same(pick, stock)) {
            return Status::pointer;
        }
        pick = candidate;
    }
    return status;
}

/** Writes every pointer field of one merged stock row and relocates what they own. */
Status Engine::merge_pointers(const Op& op, std::uint64_t row, std::size_t at) {
    const Shape& reference = shape_of(op, op.owner);
    for (std::size_t index = 0; index < reference.fields.size(); ++index) {
        const ShapeField& field = reference.fields[index];
        // A nested op's descriptor is rebuilt, not merged.
        if (rowSkip_[field.rowOffset] == kNestedByte) {
            continue;
        }
        Pointer pick{};
        Status status = pick_pointer(op, row, index, pick);
        const std::size_t holder = holder_of(op, pick.source);
        const ShapeField& own = shape_of(op, holder).fields[index];
        status = status == Status::ok ? emit(at + field.rowOffset, own, holder, pick) : status;
        if (status != Status::ok) {
            return status;
        }
    }
    return Status::ok;
}

/** Appends one input's added rows after the rows of every earlier input. */
Status Engine::write_added_rows(std::uint32_t id, std::size_t input) {
    const Op& op = ops_[id];
    const View& from = op.views[input];
    // Added rows mean the input declares the op, so it has a shape.
    for (std::uint64_t row = op.record.stockCount; row < from.count; ++row) {
        const std::size_t source = from.data + row * op.stride;
        const std::size_t at = op.header + Header::kSize + (row + op.before[input]) * op.stride;
        std::memcpy(frame_->output->data() + at, copies_[input].data() + source, op.stride);
        for (const ShapeField& field : shape_of(op, input).fields) {
            Pointer pointer{};
            const std::size_t offset = field.rowOffset;
            Status status =
                read_pointer(input, source + offset, field.kind, row_op(input, field), pointer);
            status = status == Status::ok ? emit(at + offset, field, input, pointer) : status;
            if (status != Status::ok) {
                return status;
            }
        }
    }
    return Status::ok;
}

/** Relocates the pointers in one owned array once. @param holder Input owning the shapes. */
Status
Engine::walk(std::size_t source, std::size_t holder, std::size_t field, std::uint16_t shape) {
    if (shape == kNoElementShape) {
        return Status::ok;
    }
    const Bytes bytes = source_bytes(source);
    tables::Array array{};
    if (!tables::find_optional_array_at(bytes, field, array)) {
        return Status::pointer;
    }
    if (array.count == 0 || !visited_.insert({source, array.dataOffset}).second) {
        return Status::ok;
    }
    const Shape& element = *inputs_[holder].sidecar->shape(shape);
    if (!fits(bytes, array.dataOffset, array.count * element.stride)) {
        return Status::format;
    }
    for (std::size_t index = 0; index < array.count; ++index) {
        for (const ShapeField& item : element.fields) {
            const std::size_t at = array.dataOffset + index * element.stride + item.rowOffset;
            const Status status = relocate(source, holder, at, item);
            if (status != Status::ok) {
                return status;
            }
        }
    }
    return Status::ok;
}

/** Relocates the pointers in one object once. @param holder Input owning the shapes. */
Status Engine::walk_object(std::size_t source,
                           std::size_t holder,
                           std::size_t object,
                           std::uint16_t shape) {
    if (shape == kNoElementShape || !visited_.insert({source, object}).second) {
        return Status::ok;
    }
    const Shape& layout = *inputs_[holder].sidecar->shape(shape);
    if (!fits(source_bytes(source), object, layout.stride)) {
        return Status::format;
    }
    for (const ShapeField& item : layout.fields) {
        const Status status = relocate(source, holder, object + item.rowOffset, item);
        if (status != Status::ok) {
            return status;
        }
    }
    return Status::ok;
}

/** Rewrites one pointer field of an owned element at its output address. */
Status
Engine::relocate(std::size_t source, std::size_t holder, std::size_t at, const ShapeField& field) {
    Pointer pointer{};
    std::size_t out = 0;
    Location where{};
    Status status = read_pointer(source, at, field.kind, row_op(holder, field), pointer);
    status = status == Status::ok ? map(source, at, out, where) : status;
    // Owned elements never sit inside a rebuilt array.
    if (status != Status::ok || where.op != kNoOp) {
        return status == Status::ok ? Status::pointer : status;
    }
    // A stock pointer another input edited in place cannot be relocated, so the merge falls back.
    const std::size_t size = field_size(field.kind);
    const std::byte* held = source_bytes(source).data() + at;
    if (at < stock_.size()
        && (!fits(*frame_->output, out, size)
            || std::memcmp(frame_->output->data() + out, held, size) != 0)) {
        return Status::pointer;
    }
    // A stock element is rewritten only when its target moved.
    const bool moved = at >= stock_.size() || (!pointer.null && pointer.target != pointer.raw);
    return emit(out, field, holder, pointer, moved);
}

/** @return ok, order when ordered keys do not ascend, duplicate when a new key repeats. */
Status Engine::check_keys(const Op& op) const {
    const bool unique = (op.record.flags & kUniqueKey) != 0;
    const bool ordered = (op.record.flags & kOrdered) != 0;
    if (!unique && !ordered) {
        return Status::ok;
    }
    const Bytes output(*frame_->output);
    std::vector<std::pair<std::uint32_t, bool>> keys;
    keys.reserve(static_cast<std::size_t>(op.count));
    for (std::uint64_t row = 0; row < op.count; ++row) {
        const std::size_t at = row * op.stride + op.record.keyOffset;
        const auto key = load<std::uint32_t>(output, op.header + Header::kSize + at);
        if (ordered && row != 0 && key <= keys.back().first) {
            return Status::order;
        }
        const bool added = row >= op.record.stockCount;
        keys.emplace_back(key, added || key != load<std::uint32_t>(stock_, op.stock.data + at));
    }
    // Stock may repeat a key; a repeat of an added or changed key is refused.
    std::sort(keys.begin(), keys.end());
    const auto repeat =
        std::adjacent_find(keys.begin(), keys.end(), [](const auto& first, const auto& second) {
            return first.first == second.first && (first.second || second.second);
        });
    return !unique || repeat == keys.end() ? Status::ok : Status::duplicate;
}

/** Collects the stock array spans and structural spans of every op. */
void Engine::spans(std::vector<Span>& output) const {
    output.clear();
    const auto add = [&output](std::size_t begin, std::size_t end, bool structural) {
        output.push_back(
            {static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end), structural});
    };
    if (has_size_word(stock_)) {
        add(0, reader::layout::kSizeWordSize, true);
    }
    for (const Op& op : ops_) {
        const View& from = op.stock;
        add(from.descriptor, from.descriptor + kDescriptorSize, true);
        if (from.header != 0) {
            add(from.header - Header::kMarkerBack,
                from.data + static_cast<std::size_t>(from.count) * op.stride,
                false);
            add(from.header, from.header + sizeof(std::uint64_t), true);
        }
    }
    std::sort(output.begin(), output.end(), [](const Span& first, const Span& second) {
        return std::tie(first.begin, first.end) < std::tie(second.begin, second.end);
    });
}

/** Runs the whole row merge of one entry, with the byte merge between reading and writing. */
Status Engine::merge(Frame& frame, const SpanMerge& byteMerge, std::vector<Conflict>& conflicts) {
    if (frame.output == nullptr || copies_.size() != inputs_.size()
        || frame.blobDelta.size() != inputs_.size()) {
        return Status::format;
    }
    frame_ = &frame;
    Status status = build();
    if (status == Status::ok) {
        std::vector<Span> stock;
        spans(stock);
        status = byteMerge(stock);
    }
    const std::array steps{&Engine::count_rows, &Engine::place, &Engine::index_regions};
    for (const auto step : steps) {
        status = status == Status::ok ? (this->*step)() : status;
    }
    for (std::uint32_t id = 0; status == Status::ok && id < ops_.size(); ++id) {
        status = write_op(id);
    }
    if (status != Status::ok) {
        return status;
    }
    for (const auto& [pair, bytes] : tally_) {
        conflicts.push_back({.entryIndex = entryIndex_,
                             .earlierMod = inputs_[pair.first].mod,
                             .laterMod = inputs_[pair.second].mod,
                             .bytes = bytes});
    }
    return Status::ok;
}

} // namespace

/** Reads every op once, runs the byte merge, then rebuilds the ops at the end of the output. */
Status merge_ops(Frame& frame,
                 std::span<const std::vector<std::byte>> shiftedCopies,
                 std::span<const Input> inputs,
                 std::uint32_t entryIndex,
                 const SpanMerge& bytes,
                 std::vector<Conflict>& conflicts) {
    Engine engine(frame.stock, inputs, entryIndex, shiftedCopies);
    return engine.merge(frame, bytes, conflicts);
}

} // namespace sunrise::middleware::content::packages::table_merge::internal
