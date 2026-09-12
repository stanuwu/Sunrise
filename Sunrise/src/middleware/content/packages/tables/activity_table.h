#pragma once
#include <cstring>
#include <limits>

#include "../../../../state/build_data/activities/activity_catalog.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables::activities {
/** Public activity index, NOT the package-definition index in root slot 29. */
inline constexpr std::size_t kRootSlot = 4;
inline constexpr std::uint32_t kRowClass = 0x808076FCU;
/** One index row: the definition hash, then a self-relative pointer to the record. */
inline constexpr std::size_t kIndexRowStride = 16;
inline constexpr std::size_t kIndexRecordField = 8;
/** Self-relative pointer to the internal package name, or zero when the row has none. */
inline constexpr std::size_t kRecordNameField = 0x68;
/** Activity type, a row of the activity type table. */
inline constexpr std::size_t kRecordTypeOffset = 0xDA;
inline constexpr std::size_t kRecordGameplaySettingsOffset = 0xDC;
/** Destination, a row of the destination table. */
inline constexpr std::size_t kRecordDestinationOffset = 0xE0;
/** Every field above sits inside this many bytes of a record. */
inline constexpr std::size_t kRecordReadBytes = 0xE4;

template <class T>
[[nodiscard]] bool read(std::span<const std::byte> bytes, std::size_t offset, T& value) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}
/**
 * Resolves a self-relative field.
 * @param field Offset of the 8-byte signed delta.
 * @param target Receives the resolved offset.
 * @return False when the delta is zero or the target falls outside the blob.
 */
[[nodiscard]] inline bool
relative(std::span<const std::byte> bytes, std::size_t field, std::size_t& target) noexcept {
    std::int64_t delta{};
    if (!read(bytes, field, delta) || delta == 0
        || field > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    const auto base = static_cast<std::int64_t>(field);
    if (delta < -base || delta > (std::numeric_limits<std::int64_t>::max)() - base) {
        return false;
    }
    target = static_cast<std::size_t>(base + delta);
    return target < bytes.size();
}
/**
 * Parses the dense public activity table.
 * @param bytes The table blob.
 * @param output Receives one row per ordinal.
 * @param count Receives the row count.
 * @return False when a row fails its identity check or a name is malformed.
 */
[[nodiscard]] inline bool decode(std::span<const std::byte> bytes,
                                 std::span<state::build_data::activities::Definition> output,
                                 std::size_t& count) noexcept {
    count = 0;
    Array array{};
    if (!find_array_at(bytes, kTableArrayDescriptor, array) || array.elementClass != kRowClass
        || array.count > output.size() || array.count > state::build_data::activities::kCapacity
        || array.dataOffset > bytes.size()
        || array.count > (bytes.size() - array.dataOffset) / kIndexRowStride) {
        return false;
    }
    for (std::size_t i = 0; i < array.count; ++i) {
        state::build_data::activities::Definition row{};
        row.index = static_cast<std::uint16_t>(i);
        const auto entry = array.dataOffset + i * kIndexRowStride;
        std::size_t record{};
        std::uint32_t identity{};
        // The record repeats the index row's hash; both copies must agree.
        if (!read(bytes, entry, row.hash) || row.hash == 0
            || !relative(bytes, entry + kIndexRecordField, record)
            || bytes.size() - record < kRecordReadBytes || !read(bytes, record, identity)
            || identity != row.hash
            || !read(bytes, record + kRecordGameplaySettingsOffset, row.gameplaySettingsHash)
            || !read(bytes, record + kRecordTypeOffset, row.nativeType)
            || !read(bytes, record + kRecordDestinationOffset, row.destination)) {
            return false;
        }
        std::int64_t nameDelta{};
        if (!read(bytes, record + kRecordNameField, nameDelta)) {
            return false;
        }
        if (nameDelta != 0) {
            std::size_t name{};
            if (!relative(bytes, record + kRecordNameField, name)) {
                return false;
            }
            bool terminated = false;
            for (std::size_t n = 0; n < row.package.size() && n < bytes.size() - name; ++n) {
                const auto c = static_cast<char>(bytes[name + n]);
                if (c == '\0') {
                    terminated = true;
                    break;
                }
                if (n + 1 == row.package.size()
                    || (c != '_' && (c < 'a' || c > 'z') && (c < '0' || c > '9'))) {
                    return false;
                }
                row.package[n] = c;
            }
            if (!terminated) {
                return false;
            }
        }
        output[i] = row;
    }
    count = static_cast<std::size_t>(array.count);
    return count != 0;
}
} // namespace sunrise::middleware::content::packages::tables::activities
