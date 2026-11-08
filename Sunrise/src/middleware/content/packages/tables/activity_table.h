#pragma once
#include <cstring>
#include <limits>

#include "../../../../state/build_data/activities/activity_catalog.h"
#include "../schema/generated/tag_classes_808076.h"
#include "../schema/generated/tag_classes_808077.h"
#include "definition_index_table.h"
#include "field_reader.h"

namespace sunrise::middleware::content::packages::tables::activities {
/** Public activity index, NOT the package-definition index in root slot 29. */
inline constexpr std::size_t kRootSlot =
    root_slot(offsetof(schema::generated::InvestmentRoot, slot4ActivityIndex));
inline constexpr std::uint32_t kRowClass = schema::generated::ActivityRecord::kClass;
/** One index row: the definition hash, then a self-relative pointer to the record. */
inline constexpr std::size_t kIndexRowStride = sizeof(schema::generated::ActivityRecord);
inline constexpr std::size_t kIndexRecordField =
    offsetof(schema::generated::ActivityRecord, activityPointer);
/** Self-relative pointer the route selector needs set before it plays the display row's movie. */
inline constexpr std::size_t kRecordMovieRouteField =
    offsetof(schema::generated::ActivityDefinition, movieRoute);
/** Self-relative pointer to the internal package name, or zero when the row has none. */
inline constexpr std::size_t kRecordNameField =
    offsetof(schema::generated::ActivityDefinition, internalName);
/** Activity type, a row of the activity type table. */
inline constexpr std::size_t kRecordTypeOffset =
    offsetof(schema::generated::ActivityDefinition, activityType);
inline constexpr std::size_t kRecordGameplaySettingsOffset =
    offsetof(schema::generated::ActivityDefinition, gameplaySettingsHash);
/** Destination, a row of the destination table. */
inline constexpr std::size_t kRecordDestinationOffset =
    offsetof(schema::generated::ActivityDefinition, destination);
/** The index row and its record both carry the definition hash. */
inline constexpr std::size_t kIndexHashField =
    offsetof(schema::generated::ActivityRecord, definitionHash);
inline constexpr std::size_t kRecordHashOffset =
    offsetof(schema::generated::ActivityDefinition, definitionHash);
/** A record is a whole definition, so every field above lies inside it. */
inline constexpr std::size_t kRecordReadBytes = sizeof(schema::generated::ActivityDefinition);

template <class T>
[[nodiscard]] bool read(std::span<const std::byte> bytes, std::size_t offset, T& value) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
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
    if (!find_array_at(bytes, offsetof(schema::generated::ActivityDefinitionTable, table), array)
        || array.elementClass != kRowClass || array.count > output.size()
        || array.count > state::build_data::activities::kCapacity || array.dataOffset > bytes.size()
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
        if (!read(bytes, entry + kIndexHashField, row.hash) || row.hash == 0
            || !relative(bytes, entry + kIndexRecordField, record)
            || bytes.size() - record < kRecordReadBytes
            || !read(bytes, record + kRecordHashOffset, identity) || identity != row.hash
            || !read(bytes, record + kRecordGameplaySettingsOffset, row.gameplaySettingsHash)
            || !read(bytes, record + kRecordTypeOffset, row.nativeType)
            || !read(bytes, record + kRecordDestinationOffset, row.destination)) {
            return false;
        }
        std::int64_t movieDelta{};
        std::int64_t nameDelta{};
        if (!read(bytes, record + kRecordMovieRouteField, movieDelta)
            || !read(bytes, record + kRecordNameField, nameDelta)) {
            return false;
        }
        row.movieRoute = movieDelta != 0;
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
