#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../encoding/bit_reader.h"
#include "activity_wire_codec.h"

// What the three wire-codec translation units share. The reflection walk primitives, the
// bounded selected-body reader, and the two maps that carry a dynamic array count from the
// field that declares it to the field that repeats.

namespace sunrise::middleware::bap::activity_message::wire_schema {

namespace bits = middleware::encoding::bits;
/** The catalog carries 34 distinct dynamic-count offsets. */
constexpr std::size_t kDynamicCountCapacity = 64;
constexpr std::size_t kMaximumRuntimeDepth = 64;
constexpr std::uint32_t kMaximumRuntimeSchemaFields = 512;
/** Exact catalog maximum; every walk remains bounded independently by structural steps. */
constexpr std::uint32_t kMaximumRuntimeArrayLength = 10'000;
constexpr std::size_t kMaximumRuntimeSteps = 102'400 * 8;

/** Small offset/value map used only for dynamic-array counts. Select first, then store and find. */
template <typename Offset> class DynamicCountMap {
public:
    void store(Offset offset, std::int64_t value) noexcept {
        for (std::size_t index = 0; index < count_; ++index) {
            if (values_[index].offset == offset) {
                values_[index].value = value;
                values_[index].present = true;
                return;
            }
        }
    }

    [[nodiscard]] bool find(Offset offset, std::int64_t& value) const noexcept {
        for (std::size_t index = count_; index != 0; --index) {
            if (values_[index - 1].offset == offset && values_[index - 1].present) {
                value = values_[index - 1].value;
                return true;
            }
        }
        return false;
    }

protected:
    struct Entry final {
        Offset offset{};
        std::int64_t value{};
        bool present{};
    };

    std::array<Entry, kDynamicCountCapacity> values_{};
    std::size_t count_{};
};

/** Dynamic-count map whose offsets come from the catalog's field rows. */
class MemoryMap final : public DynamicCountMap<std::int32_t> {
public:
    /** Selects only scalar offsets that a nested dynamic-count field will read. */
    [[nodiscard]] bool select(std::span<const FieldDescriptor> fields) noexcept {
        count_ = 0;
        for (const FieldDescriptor& field : fields) {
            if (field.typeCode != 1 || field.bias != 1 || field.widthOrCountOffset < 0) {
                continue;
            }
            const std::int32_t offset =
                field.absoluteStructOffset - field.structOffset + field.widthOrCountOffset;
            bool duplicate = false;
            for (std::size_t index = 0; index < count_; ++index) {
                duplicate = duplicate || values_[index].offset == offset;
            }
            if (duplicate) {
                continue;
            }
            if (count_ == values_.size()) {
                return false;
            }
            values_[count_++].offset = offset;
        }
        return true;
    }
};

/** @return Whether one reflection type code is a signed integer. */
[[nodiscard]] constexpr bool signed_type(std::uint8_t typeCode) noexcept {
    return typeCode >= 3 && typeCode <= 6;
}

/** Returns the native in-memory bit width for one reflection scalar type. */
[[nodiscard]] constexpr std::uint8_t storage_width(std::uint8_t typeCode) noexcept {
    switch (typeCode) {
    case 2:
        return 1;
    case 3:
    case 7:
        return 8;
    case 4:
    case 8:
        return 16;
    case 5:
    case 9:
    case 11:
        return 32;
    case 6:
    case 10:
        return 64;
    default:
        return 0;
    }
}

/** Types 12 and 35 are raw 64-bit words, read and written whole. */
[[nodiscard]] constexpr bool is_raw64(std::uint8_t typeCode) noexcept {
    return typeCode == 12 || typeCode == 35;
}

/** @return A low bit mask of the given width. Width 0 masks everything away. */
[[nodiscard]] constexpr std::uint64_t mask(std::uint8_t width) noexcept {
    return width == 64  ? (std::numeric_limits<std::uint64_t>::max)()
           : width == 0 ? 0
                        : (std::uint64_t{1} << width) - 1;
}

/** @return The value widened from its declared width, keeping its sign. */
[[nodiscard]] inline std::int64_t sign_extend(std::uint64_t value, std::uint8_t width) noexcept {
    if (width == 64) {
        return static_cast<std::int64_t>(value);
    }
    const std::uint64_t sign = std::uint64_t{1} << (width - 1);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

/** @return The index one past the last descendant of the field at index. */
[[nodiscard]] inline std::size_t
subtree_end(std::span<const FieldDescriptor> fields, std::size_t index, std::size_t end) noexcept {
    const std::uint8_t depth = fields[index].depth;
    std::size_t cursor = index + 1;
    while (cursor < end && fields[cursor].depth > depth) {
        ++cursor;
    }
    return cursor;
}

/** Counts only immediate children and returns the first one's index. */
[[nodiscard]] inline std::size_t direct_children(std::span<const FieldDescriptor> fields,
                                                 std::size_t index,
                                                 std::size_t end,
                                                 std::size_t& first) noexcept {
    first = end;
    const std::uint8_t wanted = static_cast<std::uint8_t>(fields[index].depth + 1);
    std::size_t count = 0;
    for (std::size_t cursor = index + 1; cursor < end;) {
        if (fields[cursor].depth == wanted) {
            if (first == end) {
                first = cursor;
            }
            ++count;
        }
        cursor = subtree_end(fields, cursor, end);
    }
    return count;
}

/** Reader view that cannot cross a caller-owned selected-body boundary. */
class RuntimeBitReader final {
public:
    RuntimeBitReader(bits::Reader& reader, std::size_t budget, std::size_t totalBits) noexcept
        : reader_(reader), remaining_(budget), totalBits_(totalBits) {}

    [[nodiscard]] bool read(std::uint8_t width, std::uint64_t& value) noexcept {
        if (width > remaining_ || !reader_.read(width, value)) {
            return false;
        }
        remaining_ -= width;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return remaining_;
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return totalBits_ - reader_.remaining_bits();
    }

private:
    bits::Reader& reader_;
    std::size_t remaining_{};
    std::size_t totalBits_{};
};

enum class RuntimeWalkStatus : std::uint8_t {
    complete,
    malformed,
    schemaUnavailable,
    unsupportedField,
    unsafeCount,
    outputTooSmall,
    missingValue,
};

/** @return The codec boundary one runtime walk status reports to the caller. */
[[nodiscard]] CodecStatus codec_status(RuntimeWalkStatus status) noexcept;

/** @return Whether one resolved schema row is safe to walk. */
[[nodiscard]] inline bool valid_runtime_schema(const runtime::SchemaView& schema) noexcept {
    return schema.row != runtime::kAbsentRuntimeRow && schema.handle != 0
           && schema.handle != runtime::kAbsentRuntimeRow
           && schema.fieldCount <= kMaximumRuntimeSchemaFields
           && schema.firstField <= (std::numeric_limits<std::uint32_t>::max)() - schema.fieldCount
           && (schema.arrayLength == 0
               || (schema.arrayLength >= 2 && schema.arrayLength <= kMaximumRuntimeArrayLength
                   && schema.fieldCount == 1));
}

/** Derives the maximum dynamic count backed by one exact nested storage region. */
[[nodiscard]] inline bool dynamic_capacity(const runtime::SchemaView& owner,
                                           const runtime::FieldView& field,
                                           const runtime::SchemaView& nested,
                                           std::uint32_t& output) noexcept {
    if (nested.arrayLength != 0) {
        output = nested.arrayLength;
        return output <= kMaximumRuntimeArrayLength;
    }
    if (nested.structSize == 0 || field.structOffset >= owner.structSize) {
        return false;
    }
    const std::uint32_t remaining = owner.structSize - field.structOffset;
    if (remaining % nested.structSize != 0) {
        return false;
    }
    output = remaining / nested.structSize;
    return output != 0 && output <= kMaximumRuntimeArrayLength;
}

/** Dynamic-count map whose offsets come from one resolved runtime schema. */
class RuntimeMemoryMap final : public DynamicCountMap<std::uint32_t> {
public:
    /** Stores the offset of every dynamic-count field the resolver can read. */
    [[nodiscard]] RuntimeWalkStatus select(const RuntimeSchemaResolver& resolver,
                                           const runtime::SchemaView& schema) noexcept {
        count_ = 0;
        if (resolver.readField == nullptr) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        for (std::uint32_t ordinal = 0; ordinal < schema.fieldCount; ++ordinal) {
            const std::uint32_t row = schema.firstField + ordinal;
            runtime::FieldView field{};
            if (!resolver.readField(resolver.context, row, field) || field.row != row) {
                return RuntimeWalkStatus::schemaUnavailable;
            }
            if (field.typeCode != 1 || field.biasOrDynamic != 1 || field.widthOrCountOffset < 0) {
                continue;
            }
            const auto offset = static_cast<std::uint32_t>(field.widthOrCountOffset);
            bool duplicate = false;
            for (std::size_t index = 0; index < count_; ++index) {
                duplicate = duplicate || values_[index].offset == offset;
            }
            if (duplicate) {
                continue;
            }
            if (count_ == values_.size()) {
                return RuntimeWalkStatus::unsafeCount;
            }
            values_[count_++].offset = offset;
        }
        return RuntimeWalkStatus::complete;
    }
};

struct RuntimeOccurrence final {
    std::uint32_t fieldRow{runtime::kAbsentRuntimeRow};
    std::uint32_t next{};
};

/** Occurrence numbering and step budget shared by the selected-schema reader and writer. */
class RuntimeWalkState final {
public:
    /** @return False once the walk has taken more structural steps than a body may need. */
    [[nodiscard]] bool step() noexcept {
        return ++steps_ <= kMaximumRuntimeSteps;
    }

    /** @return The next flattened occurrence ordinal for one field row, counting from zero. */
    [[nodiscard]] std::uint32_t next_occurrence(std::uint32_t fieldRow) noexcept {
        for (std::size_t index = 0; index < count_; ++index) {
            if (occurrences_[index].fieldRow == fieldRow) {
                return occurrences_[index].next++;
            }
        }
        if (count_ == occurrences_.size()) {
            overflowed_ = true;
            return 0;
        }
        occurrences_[count_].fieldRow = fieldRow;
        occurrences_[count_].next = 1;
        ++count_;
        return 0;
    }

    /** @return True once more field rows were visited than the occurrence table can name. */
    [[nodiscard]] bool overflowed() const noexcept {
        return overflowed_;
    }

private:
    std::array<RuntimeOccurrence, kRuntimeValueCapacity> occurrences_{};
    std::size_t count_{};
    std::size_t steps_{};
    bool overflowed_{};
};

/** @return The wire width one selected field declares, or 0 when it declares none. */
[[nodiscard]] inline std::uint8_t runtime_declared_width(const runtime::FieldView& field) noexcept {
    if (field.typeCode == 2) {
        return 1;
    }
    if (field.typeCode == 11) {
        return field.parameter2 > 0 && field.parameter2 < 32
                   ? static_cast<std::uint8_t>(field.parameter2)
                   : 32;
    }
    return field.widthOrCountOffset > 0 && field.widthOrCountOffset <= 64
               ? static_cast<std::uint8_t>(field.widthOrCountOffset)
               : 0;
}

/** @return The explicit codec boundary one non-reflection layout maps to. */
[[nodiscard]] CodecStatus layout_status(LayoutKind layout) noexcept;

/** @return Whether the message is the one-field runtime-selected root grammar. */
[[nodiscard]] bool runtime_selected_root(const MessageDescriptor& message) noexcept;

} // namespace sunrise::middleware::bap::activity_message::wire_schema
