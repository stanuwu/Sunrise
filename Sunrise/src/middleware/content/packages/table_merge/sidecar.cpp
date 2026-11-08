#include "sidecar.h"

#include <algorithm>
#include <utility>

#include "internal.h"

namespace sunrise::middleware::content::packages::table_merge {
namespace {

using internal::Cursor;

/** Pointer reach of a shape that holds no row pointer. */
constexpr std::int64_t kNoPointer = -1;
/** Every OpRecord flag bit version 2 defines. */
constexpr std::uint8_t kOpFlags = kUniqueKey | kOrdered;

/**
 * Reads one shape; shape refs must point at later shapes, so shapes form no cycle.
 * @param objects False for a version 2 file, whose object pointers name no shape.
 * @return ok, or format when a field breaks a layout rule.
 */
[[nodiscard]] Status read_shape(
    Cursor& cursor, std::uint32_t index, std::uint32_t shapeCount, bool objects, Shape& output) {
    ShapeHeader header{};
    if (!cursor.take(header) || header.stride == 0
        || !cursor.fits(header.fieldCount, sizeof(ShapeField))) {
        return Status::format;
    }
    output.stride = header.stride;
    output.fields.resize(header.fieldCount);
    std::uint64_t end = 0;
    for (ShapeField& field : output.fields) {
        if (!cursor.take(field) || field.reserved != 0
            || (field.kind != FieldKind::ownedArray && field.kind != FieldKind::rowPointer
                && field.kind != FieldKind::objectPointer)) {
            return Status::format;
        }
        if (!objects && field.kind == FieldKind::objectPointer && field.ref != kNoElementShape) {
            return Status::format;
        }
        const std::uint64_t fieldEnd = std::uint64_t{field.rowOffset} + field_size(field.kind);
        if (field.rowOffset < end || fieldEnd > header.stride) {
            return Status::format;
        }
        end = fieldEnd;
        if (field.kind != FieldKind::rowPointer && field.ref != kNoElementShape
            && (field.ref <= index || field.ref >= shapeCount)) {
            return Status::format;
        }
    }
    return Status::ok;
}

/**
 * Per shape, the largest op a row pointer in it or in what it owns names, or -1.
 * Shape refs only point forward, so one backward pass sees every child first.
 */
[[nodiscard]] std::vector<std::int64_t> pointer_reach(const std::vector<Shape>& shapes) {
    std::vector<std::int64_t> reach(shapes.size(), kNoPointer);
    for (std::size_t index = shapes.size(); index-- > 0;) {
        for (const ShapeField& field : shapes[index].fields) {
            if (field.kind == FieldKind::rowPointer) {
                reach[index] = (std::max)(reach[index], std::int64_t{field.ref});
            } else if (field.ref != kNoElementShape) {
                reach[index] = (std::max)(reach[index], reach[field.ref]);
            }
        }
    }
    return reach;
}

/** @return True when one op obeys its layout rules; earlier ops are already checked. */
[[nodiscard]] bool op_valid(const OpRecord& op,
                            std::size_t index,
                            std::span<const OpRecord> earlier,
                            const std::vector<Shape>& shapes,
                            const EntryHeader& entry) noexcept {
    if ((op.kind != OpKind::append && op.kind != OpKind::sortedMap) || (op.flags & ~kOpFlags) != 0
        || (op.keyWidth != 0 && op.keyWidth != kKeyWidth) || op.reserved != 0
        || op.shape >= shapes.size() || op.modCount < op.stockCount || op.modCount > op.limit) {
        return false;
    }
    const std::uint32_t stride = shapes[op.shape].stride;
    const bool keyed = op.flags != 0 || op.kind == OpKind::sortedMap;
    if ((keyed && op.keyWidth == 0)
        || (op.keyWidth != 0 && std::uint64_t{op.keyOffset} + op.keyWidth > stride)) {
        return false;
    }
    const std::uint64_t descriptorEnd = std::uint64_t{op.descriptorOffset} + kDescriptorSize;
    if (op.parentOp == kTopLevelOp) {
        return descriptorEnd <= entry.builtSize;
    }
    // A parent comes first, so nesting forms no cycle.
    if (op.parentOp >= index) {
        return false;
    }
    const OpRecord& parent = earlier[op.parentOp];
    return op.parentRow < parent.modCount && descriptorEnd <= shapes[parent.shape].stride;
}

/** @return True when the counts obey the rules of the entry's kind. */
[[nodiscard]] bool kind_valid(const EntryHeader& header) noexcept {
    switch (header.kind) {
    case EntryKind::opaque:
        return header.diffCount == 0 && header.opCount == 0;
    case EntryKind::table:
        // Only ops move pointers into added bytes, so a table without ops cannot grow.
        return header.stockSize > 0 && header.builtSize >= header.stockSize
               && (header.opCount != 0 || header.builtSize == header.stockSize);
    case EntryKind::manifest:
        return header.diffCount == 0 && header.opCount == 0 && header.siteCount == 0;
    case EntryKind::added:
        return header.stockSize == 0 && header.diffCount == 0 && header.opCount == 0;
    }
    return false;
}

/** @return ok, or format when a diff leaves the stock region or overlaps the one before. */
[[nodiscard]] Status read_diffs(Cursor& cursor, Entry& entry) {
    if (!cursor.fits(entry.header.diffCount, sizeof(DiffHeader))) {
        return Status::format;
    }
    entry.diffs.resize(entry.header.diffCount);
    std::uint64_t end = 0;
    for (Diff& diff : entry.diffs) {
        DiffHeader header{};
        if (!cursor.take(header) || header.length == 0 || header.offset < end) {
            return Status::format;
        }
        end = std::uint64_t{header.offset} + header.length;
        if (end > entry.header.stockSize || !cursor.take(header.length, diff.old)) {
            return Status::format;
        }
        diff.offset = header.offset;
    }
    return Status::ok;
}

/** @return ok, or format when an op breaks a layout rule. */
[[nodiscard]] Status read_ops(Cursor& cursor,
                              const std::vector<Shape>& shapes,
                              std::span<const std::int64_t> reach,
                              Entry& entry) {
    if (!cursor.fits(entry.header.opCount, sizeof(OpRecord))) {
        return Status::format;
    }
    entry.ops.resize(entry.header.opCount);
    for (std::size_t index = 0; index < entry.ops.size(); ++index) {
        if (!cursor.take(entry.ops[index])
            || !op_valid(
                entry.ops[index], index, std::span(entry.ops).first(index), shapes, entry.header)
            || reach[entry.ops[index].shape] >= std::int64_t{entry.header.opCount}) {
            return Status::format;
        }
    }
    return Status::ok;
}

/** @return ok, or format when a site overlaps, leaves the entry or names no target. */
[[nodiscard]] Status read_sites(Cursor& cursor, std::uint32_t targetCount, Entry& entry) {
    if (!cursor.fits(entry.header.siteCount, sizeof(SiteRecord))) {
        return Status::format;
    }
    entry.sites.resize(entry.header.siteCount);
    std::uint64_t end = 0;
    for (SiteRecord& site : entry.sites) {
        if (!cursor.take(site)
            || (site.width != kByteSite && site.width != kShortSite && site.width != kWordSite)
            || (site.flags & ~kSignedSite) != 0 || site.offset < end) {
            return Status::format;
        }
        end = std::uint64_t{site.offset} + site.width;
        const bool local = (site.target & kOpLocalTarget) != 0;
        if (end > entry.header.builtSize
            || (local
                && static_cast<std::uint32_t>(site.target & kOpLocalMask) >= entry.header.opCount)
            || (!local && site.target >= targetCount)) {
            return Status::format;
        }
    }
    return Status::ok;
}

/** @return ok, or the first broken rule of one entry. */
[[nodiscard]] Status read_entry(Cursor& cursor,
                                const std::vector<Shape>& shapes,
                                std::span<const std::int64_t> reach,
                                std::uint32_t targetCount,
                                Entry& entry) {
    EntryHeader& header = entry.header;
    if (!cursor.take(header) || header.reserved != std::array<std::uint8_t, 3>{}
        || header.reserved2 != 0 || !kind_valid(header)) {
        return Status::format;
    }
    Status status = read_diffs(cursor, entry);
    if (status == Status::ok) {
        status = read_ops(cursor, shapes, reach, entry);
    }
    return status == Status::ok ? read_sites(cursor, targetCount, entry) : status;
}

/** @return ok, or format when targets repeat or count down. */
[[nodiscard]] Status
read_targets(Cursor& cursor, std::uint32_t count, std::vector<TargetRecord>& targets) {
    if (!cursor.fits(count, sizeof(TargetRecord))) {
        return Status::format;
    }
    targets.resize(count);
    for (TargetRecord& target : targets) {
        if (!cursor.take(target) || target.modCount < target.stockCount) {
            return Status::format;
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> keys;
    keys.reserve(targets.size());
    for (const TargetRecord& target : targets) {
        keys.emplace_back(target.tableTag, target.descriptorOffset);
    }
    std::sort(keys.begin(), keys.end());
    return std::adjacent_find(keys.begin(), keys.end()) == keys.end() ? Status::ok : Status::format;
}

} // namespace

/** Entries are sorted by index, so a binary search finds one. */
const Entry* Sidecar::entry(std::uint32_t index) const noexcept {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), index, [](const Entry& item, std::uint32_t value) {
            return item.header.entryIndex < value;
        });
    return at != entries_.end() && at->header.entryIndex == index ? &*at : nullptr;
}

/** Parses the whole file before the output sees any of it. */
Status parse(std::vector<std::byte> file,
             std::uint16_t packageId,
             std::uint16_t patchId,
             std::span<const std::byte, kMetadataDigestSize> metadataDigest,
             Sidecar& output) noexcept {
    output = Sidecar{};
    try {
        Sidecar result;
        result.file_ = std::move(file);
        if (result.file_.size() > kSidecarByteLimit) {
            return Status::format;
        }
        Cursor cursor(result.file_);
        FileHeader& header = result.header_;
        if (!cursor.take(header) || header.magic != kSidecarMagic) {
            return Status::format;
        }
        // A new version may define flag bits, so the version is checked first.
        if (header.version != kSidecarVersion && header.version != kOldestSidecarVersion) {
            return Status::version;
        }
        if (header.flags != 0) {
            return Status::format;
        }
        if (header.packageId != packageId || header.patchId != patchId
            || !std::equal(
                metadataDigest.begin(), metadataDigest.end(), header.metadataDigest.begin())) {
            return Status::binding;
        }
        if (header.reserved != 0
            || read_targets(cursor, header.targetCount, result.targets_) != Status::ok
            || !cursor.fits(header.shapeCount, sizeof(ShapeHeader))) {
            return Status::format;
        }
        result.shapes_.resize(header.shapeCount);
        for (std::uint32_t index = 0; index < header.shapeCount; ++index) {
            if (read_shape(cursor,
                           index,
                           header.shapeCount,
                           header.version != kOldestSidecarVersion,
                           result.shapes_[index])
                != Status::ok) {
                return Status::format;
            }
        }
        if (!cursor.fits(header.entryCount, sizeof(EntryHeader))) {
            return Status::format;
        }
        const std::vector<std::int64_t> reach = pointer_reach(result.shapes_);
        result.entries_.resize(header.entryCount);
        for (std::size_t index = 0; index < result.entries_.size(); ++index) {
            Entry& entry = result.entries_[index];
            if (read_entry(cursor, result.shapes_, reach, header.targetCount, entry) != Status::ok
                || (index != 0
                    && entry.header.entryIndex <= result.entries_[index - 1].header.entryIndex)) {
                return Status::format;
            }
        }
        if (!cursor.done()) {
            return Status::format;
        }
        output = std::move(result);
        return Status::ok;
    } catch (...) {
        return Status::capacity;
    }
}

} // namespace sunrise::middleware::content::packages::table_merge
