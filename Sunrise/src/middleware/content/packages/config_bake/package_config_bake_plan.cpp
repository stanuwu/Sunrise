#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "../../../crypto/sha1.h"
#include "../../../crypto/sha256.h"
#include "../../cui/config_scalar.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::config_bake::internal {
namespace {

/** Entry type occupies seven bits above the subtype word. */
constexpr unsigned kTypeShift = 9;
constexpr std::uint32_t kTypeMask = 0x7F;
/** Structured entry types carry their native class directly. */
constexpr std::uint32_t kTagType = 8, kWideTagType = 16;

/**
 * Validated offsets preserve the exact integer and float bits and the native byte boolean.
 * @param entry Prepared scalar values.
 * @param bytes Private plaintext entry buffer.
 */
void apply_values(const Entry& entry, std::span<std::byte> bytes) noexcept {
    for (const auto& item : entry.values) {
        switch (item.value.type) {
        case state::mods::ConfigType::integer:
            store<std::int32_t>(bytes, item.site.offset, item.value.integer);
            break;
        case state::mods::ConfigType::floating:
            store<float>(bytes, item.site.offset, item.value.floating);
            break;
        case state::mods::ConfigType::boolean:
            store<std::uint8_t>(bytes, item.site.offset, item.value.boolean ? 1U : 0U);
            break;
        }
    }
}

/**
 * The entry's plaintext before values: its replacement, or its source bytes.
 * @param plan Held source files.
 * @param entry Packed entry.
 * @param bytes Receives the bytes.
 * @return ok, or why the source bytes cannot be read at the planned size.
 */
[[nodiscard]] Status
entry_bytes(const Prepared& plan, const Entry& entry, std::vector<std::byte>& bytes) {
    if (!entry.replacement.empty()) {
        bytes.assign(entry.replacement.begin(), entry.replacement.end());
        return Status::ok;
    }
    const Status status = read_entry(plan, entry.index, bytes);
    return status == Status::ok && bytes.size() != entry.size ? Status::blocks : status;
}

/**
 * Every binding for an entry must describe the same exact bytes and native class.
 * A replaced entry's bindings describe the replacement bytes.
 * @param plan Held source files.
 * @param entry Grouped sites.
 * @return The first entry identity, digest or scalar-layout failure.
 */
[[nodiscard]] Status validate_entry(const Prepared& plan, const Entry& entry) {
    if (entry.values.empty()) {
        return Status::ok;
    }
    layout::EntryRecord record{};
    std::memcpy(
        &record, tables::entry_bytes(plan.source.package, entry.index).data(), sizeof record);
    const auto type = (record.typeInfo >> kTypeShift) & kTypeMask;
    if ((type != kTagType && type != kWideTagType)
        || record.reference != state::mods::kConfigCuiResourceClass
        || (entry.replacement.empty() && layout::placement(record).size != entry.size)) {
        return Status::source;
    }
    if (entry.values.size() > state::mods::kConfigBindingLimit) {
        return Status::capacity;
    }
    std::vector<state::mods::ConfigBinding> sites;
    sites.reserve(entry.values.size());
    const auto& expected = entry.values.front().site.sourceSha256;
    for (const auto& item : entry.values) {
        if (item.site.classId != record.reference || item.site.size != entry.size
            || item.site.sourceSha256 != expected || item.site.type != item.value.type) {
            return Status::source;
        }
        switch (item.value.type) {
        case state::mods::ConfigType::integer:
        case state::mods::ConfigType::boolean:
            break;
        case state::mods::ConfigType::floating:
            if (!std::isfinite(item.value.floating)) {
                return Status::layout;
            }
            break;
        default:
            return Status::layout;
        }
        sites.push_back(item.site);
    }
    std::vector<std::byte> bytes;
    const Status status = entry_bytes(plan, entry, bytes);
    if (status != Status::ok) {
        return status;
    }
    crypto::sha256::Digest actual{};
    if (!crypto::sha256::hash(bytes, actual) || actual != expected) {
        return Status::source;
    }
    return cui::validate_config_scalars(bytes, sites) ? Status::ok : Status::layout;
}

/**
 * Replacements must be sorted by unique index, name a source entry and fit one entry.
 * @return ok, layout for an order break, identity or capacity.
 */
[[nodiscard]] Status check_replacements(std::span<const Replacement> replacements,
                                        const tables::Package& source) noexcept {
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        const Replacement& item = replacements[i];
        if (i != 0 && item.index <= replacements[i - 1].index) {
            return Status::layout;
        }
        if (item.index >= source.entryCount) {
            return Status::identity;
        }
        if (item.bytes.empty() || item.bytes.size() > state::mods::kConfigEntryByteLimit) {
            return Status::capacity;
        }
    }
    return Status::ok;
}

/**
 * Sorts values by tag and offset; every tag must name an entry of the source package.
 * @return ok, identity or capacity.
 */
[[nodiscard]] Status order_values(std::span<const Value> values,
                                  const tables::Package& source,
                                  std::vector<Value>& output) {
    if (values.size() > kValueLimit) {
        return Status::capacity;
    }
    output.assign(values.begin(), values.end());
    std::sort(output.begin(), output.end(), [](const Value& a, const Value& b) {
        return a.site.tag < b.site.tag
               || (a.site.tag == b.site.tag && a.site.offset < b.site.offset);
    });
    for (const auto& item : output) {
        if (item.site.tag < layout::kTagBase
            || ((item.site.tag - layout::kTagBase) >> layout::kTagEntryBits) != source.packageId
            || (item.site.tag & layout::kTagEntryMask) >= source.entryCount) {
            return Status::identity;
        }
    }
    return Status::ok;
}

/** @return The entry index of one value. */
[[nodiscard]] std::uint32_t value_index(const Value& value) noexcept {
    return value.site.tag & layout::kTagEntryMask;
}

/**
 * Starts the next packed entry: the lowest index among the remaining values and replacements.
 * @return ok, or capacity when the first value of a source entry names an unusable size.
 */
[[nodiscard]] Status next_entry(std::span<const Value> values,
                                std::span<const Replacement> replacements,
                                Entry& entry) noexcept {
    const bool value = !values.empty();
    const bool replaced =
        !replacements.empty() && (!value || replacements.front().index <= value_index(values[0]));
    if (replaced) {
        entry.index = replacements.front().index;
        entry.replacement = replacements.front().bytes;
        entry.size = static_cast<std::uint32_t>(entry.replacement.size());
        return Status::ok;
    }
    entry.index = value_index(values[0]);
    entry.size = values[0].site.size;
    return entry.size == 0 || entry.size > state::mods::kConfigEntryByteLimit ? Status::capacity
                                                                              : Status::ok;
}

/**
 * Index order fixes where each entry lands in the new blocks.
 * @param values Caller values, copied into the plan.
 * @param replacements Whole entries, packed with the value entries in index order.
 * @param output Held sources and resulting entry groups.
 * @return The first entry, size or site failure.
 */
[[nodiscard]] Status group_entries(std::span<const Value> values,
                                   std::span<const Replacement> replacements,
                                   Prepared& output) {
    if (values.empty() && replacements.empty()) {
        return Status::layout;
    }
    const auto& source = output.source.package;
    std::vector<Value> ordered;
    Status status = check_replacements(replacements, source);
    if (status == Status::ok) {
        status = order_values(values, source, ordered);
    }
    std::span<const Value> pendingValues(ordered);
    std::uint64_t position = 0;
    while (status == Status::ok && (!pendingValues.empty() || !replacements.empty())) {
        Entry entry{};
        status = next_entry(pendingValues, replacements, entry);
        if (!entry.replacement.empty()) {
            replacements = replacements.subspan(1);
        }
        while (!pendingValues.empty() && value_index(pendingValues.front()) == entry.index) {
            entry.values.push_back(pendingValues.front());
            pendingValues = pendingValues.subspan(1);
        }
        position = align(position, kEntryAlignment);
        entry.position = position;
        position += entry.size;
        if (source.blockCount + (position + layout::kBlockSize - 1) / layout::kBlockSize
            > tables::kBlockLimit) {
            status = Status::capacity;
        }
        output.entries.push_back(std::move(entry));
    }
    output.newBlocks =
        static_cast<std::uint32_t>((position + layout::kBlockSize - 1) / layout::kBlockSize);
    for (const auto& entry : output.entries) {
        if (status != Status::ok) {
            break;
        }
        status = validate_entry(output, entry);
    }
    return status;
}

/**
 * Original active records stay byte-exact; only selected entry placements and new blocks change.
 * @param output Validated sources and packed entries.
 * @return False when the grown metadata or output file exceeds package limits.
 */
[[nodiscard]] bool layout_output(Prepared& output) {
    const auto& original = output.source.package;
    const std::size_t count = original.blockCount + output.newBlocks;
    const std::size_t capacity = (std::max)(original.blockCapacity, count);
    const std::size_t shift = (capacity - original.blockCapacity) * layout::kBlockRecordSize;
    if (original.metadata.size() + shift > tables::kRegionLimit
        || !tables::grow_block_table(original, count, output.metadata)) {
        return false;
    }
    for (const auto& entry : output.entries) {
        const std::uint64_t start = original.blockCount + entry.position / layout::kBlockSize;
        store<std::uint64_t>(
            output.metadata,
            MetadataField::kEntryTable + entry.index * layout::kEntryRecordSize
                + tables::EntryRecordField::kPlacement,
            tables::encode_placement(start, entry.position % layout::kBlockSize, entry.size));
    }
    output.metadataOffset = align(output.source.size);
    output.blocksOffset = align(output.metadataOffset + output.metadata.size());
    output.footer = output.blocksOffset + std::uint64_t{output.newBlocks} * layout::kBlockSize;
    output.fileSize = output.footer + kDiskAlignment;
    return output.fileSize <= (std::numeric_limits<std::uint32_t>::max)();
}

/** @param context Prepared metadata. @param index New block ordinal. @param bytes Full block. */
[[nodiscard]] bool
describe_block(void* context, std::uint32_t index, std::span<const std::byte> bytes) {
    auto& output = *static_cast<Prepared*>(context);
    layout::BlockRecord record{};
    record.offset = static_cast<std::uint32_t>(output.blocksOffset + index * layout::kBlockSize);
    record.size = static_cast<std::uint32_t>(bytes.size());
    record.patchId = output.source.package.patchId;
    if (!crypto::sha1::hash(bytes, record.sha1)) {
        return false;
    }
    std::memcpy(output.metadata.data() + output.source.package.blockTable
                    + (output.source.package.blockCount + index) * layout::kBlockRecordSize,
                &record,
                sizeof record);
    return true;
}

} // namespace

/** At most one entry and one block are decoded at a time, regardless of total configured bytes. */
bool blocks(const Prepared& plan, BlockVisitor visitor, void* context) {
    std::vector<std::byte> buffer(layout::kBlockSize), bytes;
    std::uint32_t block = 0;
    bool pending = false;
    for (const auto& entry : plan.entries) {
        if (entry_bytes(plan, entry, bytes) != Status::ok) {
            return false;
        }
        apply_values(entry, bytes);
        if (entry.position / layout::kBlockSize != block) {
            if (!pending || !visitor(context, block++, buffer)) {
                return false;
            }
            std::fill(buffer.begin(), buffer.end(), std::byte{});
            pending = false;
        }
        std::size_t copied = 0;
        auto offset = static_cast<std::size_t>(entry.position % layout::kBlockSize);
        while (copied < bytes.size()) {
            const auto take = (std::min)(bytes.size() - copied, layout::kBlockSize - offset);
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(copied),
                        take,
                        buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            copied += take;
            offset += take;
            pending = true;
            if (offset == layout::kBlockSize) {
                if (!visitor(context, block++, buffer)) {
                    return false;
                }
                std::fill(buffer.begin(), buffer.end(), std::byte{});
                offset = 0;
                pending = false;
            }
        }
    }
    if (pending && !visitor(context, block++, buffer)) {
        return false;
    }
    return block == plan.newBlocks;
}

/** All public operations prepare the same bytes before opening a cache for publication. */
Status prepare(const wchar_t* source,
               std::span<const Parent> parents,
               std::span<const Value> values,
               std::span<const Replacement> replacements,
               Prepared& output) {
    Status status = read_inputs(source, parents, output);
    if (status != Status::ok) {
        return status;
    }
#if defined(SUNRISE_CONFIG_BAKE_TEST)
    sources_held_for_test(output);
#endif
    status = group_entries(values, replacements, output);
    if (status != Status::ok) {
        return status;
    }
    if (!layout_output(output)) {
        return Status::capacity;
    }
    if (!blocks(output, &describe_block, &output)) {
        return Status::blocks;
    }
    output.header = output.source.package.header;
    crypto::sha1::Digest digest{};
    if (!crypto::sha1::hash(output.metadata, digest)) {
        return Status::writeFailed;
    }
    store<std::uint32_t>(output.header,
                         HeaderField::kBlockCount,
                         output.source.package.blockCount + output.newBlocks);
    store<std::uint32_t>(output.header,
                         HeaderField::kMetadataOffset,
                         static_cast<std::uint32_t>(output.metadataOffset));
    store<std::uint32_t>(output.header,
                         HeaderField::kMetadataSize,
                         static_cast<std::uint32_t>(output.metadata.size()));
    std::copy(digest.begin(), digest.end(), output.header.begin() + HeaderField::kMetadataDigest);
    store<std::uint32_t>(
        output.header, HeaderField::kFooterOffset, static_cast<std::uint32_t>(output.footer));
    store<std::uint32_t>(
        output.header, HeaderField::kFileSize, static_cast<std::uint32_t>(output.fileSize));
    return Status::ok;
}

} // namespace sunrise::middleware::content::packages::config_bake::internal
