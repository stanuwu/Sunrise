#pragma once
#include "../schema/generated/tag_classes_808000.h"
#include "../schema/generated/tag_classes_80805e.h"
#include "../schema/generated/tag_classes_80805f.h"
#include "../schema/generated/tag_classes_80809a.h"
#include "activity_table.h"

namespace sunrise::middleware::content::packages::tables::activities {
/** Bank ordinal of a string reference that names no bank. */
inline constexpr std::uint32_t kNoBank = 0xFFFFU;
/** Category of a display row that names none. */
inline constexpr std::uint16_t kNoCategory = 0xFFFFU;
/** One display index row: the definition hash, then a self-relative pointer to the record. */
inline constexpr std::size_t kDisplayRowStride = sizeof(schema::generated::ActivityClientIndexRow);
inline constexpr std::size_t kDisplayRecordField =
    offsetof(schema::generated::ActivityClientIndexRow, activityPointer);
/** The display record starts with a self-relative pointer to its text block. */
inline constexpr std::size_t kDisplayTextField =
    offsetof(schema::generated::ActivityTextRecord, textBlock);
inline constexpr std::size_t kDisplayCategoryOffset =
    offsetof(schema::generated::ActivityTextRecord, category);
/** Package name hash of the row's pre-rendered movie; the empty-string hash names none. */
inline constexpr std::size_t kDisplayMovieOffset =
    offsetof(schema::generated::ActivityTextRecord, movie);
/** String references inside the text block, and the Director style tag after them. */
inline constexpr std::size_t kTextTitleBankOffset =
    offsetof(schema::generated::ActivityDisplayRow, titleBank);
inline constexpr std::size_t kTextTitleHashOffset =
    offsetof(schema::generated::ActivityDisplayRow, titleHash);
inline constexpr std::size_t kTextDescriptionBankOffset =
    offsetof(schema::generated::ActivityDisplayRow, descriptionBank);
inline constexpr std::size_t kTextDescriptionHashOffset =
    offsetof(schema::generated::ActivityDisplayRow, descriptionHash);
inline constexpr std::size_t kTextExpansionBankOffset =
    offsetof(schema::generated::ActivityDisplayRow, expansionBank);
inline constexpr std::size_t kTextExpansionHashOffset =
    offsetof(schema::generated::ActivityDisplayRow, expansionHash);
inline constexpr std::size_t kTextStyleOffset =
    offsetof(schema::generated::ActivityDisplayRow, styleHash);
/** Client activity-type rows, with the type name reference after the row hash. */
inline constexpr std::size_t kTypeRowStride = sizeof(schema::generated::ActivityTypeRow);
inline constexpr std::size_t kTypeNameBankOffset =
    offsetof(schema::generated::ActivityTypeRow, typeNameBank);
inline constexpr std::size_t kTypeNameHashOffset =
    offsetof(schema::generated::ActivityTypeRow, typeNameHash);
/** Rows the type table may declare; the type byte on an activity record is one of them. */
inline constexpr std::size_t kTypeRowLimit = 256;

/** Bank string hashes are packed u32 rows. */
inline constexpr std::size_t kBankHashStride = sizeof(schema::generated::SU32HashElement);
/** The language blob's combination array descriptor, and its rows. */
inline constexpr std::size_t kLanguageCombinationDescriptor =
    offsetof(schema::generated::SLocalizedStringsData, strings);
inline constexpr std::size_t kCombinationStride = sizeof(schema::generated::SStringCombination);
/** A combination starts with a self-relative pointer to its parts, then the part count. */
inline constexpr std::size_t kCombinationPartsField =
    offsetof(schema::generated::SStringCombination, firstPart);
inline constexpr std::size_t kCombinationCountOffset =
    offsetof(schema::generated::SStringCombination, partCount);
/** Parts one string may combine; the widest installed string is far below it. */
inline constexpr std::int64_t kCombinationPartLimit = 32;
/** A part: a self-relative pointer to its bytes, their count, and the per-byte shift. */
inline constexpr std::size_t kPartStride = sizeof(schema::generated::SStringPart);
inline constexpr std::size_t kPartDataField = offsetof(schema::generated::SStringPart, text);
inline constexpr std::size_t kPartByteCountOffset =
    offsetof(schema::generated::SStringPart, byteLength);
inline constexpr std::size_t kPartShiftOffset =
    offsetof(schema::generated::SStringPart, cipherShift);
/** UTF-8 lead-byte thresholds for two, three and four byte sequences, and the first invalid lead.
 */
inline constexpr std::uint8_t kUtf8TwoByteLead = 0xC0;
inline constexpr std::uint8_t kUtf8ThreeByteLead = 0xE0;
inline constexpr std::uint8_t kUtf8FourByteLead = 0xF0;
inline constexpr std::uint8_t kUtf8InvalidLead = 0xF5;

struct StringRef {
    std::uint32_t bank{kNoBank}, hash{kNoPlugSource};
};
struct DisplayRefs {
    StringRef title{}, description{}, type{}, expansion{};
    std::uint16_t category{kNoCategory};
    std::uint32_t style{kNoPlugSource};
    std::uint32_t movie{state::build_data::activities::kNoMovie};
};
/**
 * Reads one string reference: a 16-bit bank ordinal, then its string hash.
 * @param blob The blob holding the reference.
 * @param bankAt Offset of the bank ordinal.
 * @param hashAt Offset of the string hash.
 * @param output Receives the reference; unchanged on failure.
 * @return False when either field lies outside the blob.
 */
[[nodiscard]] inline bool read_string_ref(std::span<const std::byte> blob,
                                          std::size_t bankAt,
                                          std::size_t hashAt,
                                          StringRef& output) noexcept {
    std::uint16_t bank{};
    std::uint32_t hash{};
    if (!read(blob, bankAt, bank) || !read(blob, hashAt, hash)) {
        return false;
    }
    output = {bank, hash};
    return true;
}
/**
 * Reads the string references of every activity's display row.
 * Client slot 3 uses the exact same ordinal AND authored identity as public slot 4.
 * @param display The client activity blob.
 * @param types The client activity-type blob.
 * @param rows The public activity rows, in ordinal order.
 * @param output Receives one entry per row.
 * @return False when a display row disagrees with its public row.
 */
[[nodiscard]] inline bool
display_refs(std::span<const std::byte> display,
             std::span<const std::byte> types,
             std::span<const state::build_data::activities::Definition> rows,
             std::span<DisplayRefs> output) noexcept {
    Array a{}, t{};
    if (!find_array_at(display, offsetof(schema::generated::ActivityClientTable, table), a)
        || a.count != rows.size() || output.size() < rows.size()
        || !find_array_at(types, offsetof(schema::generated::ActivityTypeTable, table), t)
        || t.count > kTypeRowLimit || a.dataOffset > display.size()
        || a.count > (display.size() - a.dataOffset) / kDisplayRowStride
        || t.dataOffset > types.size()
        || t.count > (types.size() - t.dataOffset) / kTypeRowStride) {
        return false;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto entry = a.dataOffset + i * kDisplayRowStride;
        std::uint32_t hash{};
        std::size_t record{}, text{};
        output[i] = {};
        if (!read(display, entry, hash) || hash != rows[i].hash
            || !relative(display, entry + kDisplayRecordField, record)
            || !relative(display, record + kDisplayTextField, text)
            || !read_string_ref(
                display, text + kTextTitleBankOffset, text + kTextTitleHashOffset, output[i].title)
            || !read_string_ref(display,
                                text + kTextDescriptionBankOffset,
                                text + kTextDescriptionHashOffset,
                                output[i].description)
            || !read_string_ref(display,
                                text + kTextExpansionBankOffset,
                                text + kTextExpansionHashOffset,
                                output[i].expansion)
            || !read(display, record + kDisplayCategoryOffset, output[i].category)
            || !read(display, text + kTextStyleOffset, output[i].style)
            || !read(display, record + kDisplayMovieOffset, output[i].movie)) {
            return false;
        }
        const std::size_t typeRow = t.dataOffset + rows[i].nativeType * kTypeRowStride;
        if (rows[i].nativeType < t.count
            && !read_string_ref(types,
                                typeRow + kTypeNameBankOffset,
                                typeRow + kTypeNameHashOffset,
                                output[i].type)) {
            return false;
        }
    }
    return true;
}
/**
 * Decodes one bank-scoped English string.
 * A hash never resolves against another bank. Oversize or malformed text is unavailable, not
 * truncated.
 * @param container The bank's string-hash blob.
 * @param language The bank's English language blob.
 * @param hash The string hash to decode.
 * @param output Receives the text and its null; empty on failure.
 * @return True when the whole string fit.
 */
[[nodiscard]] inline bool localized_string(std::span<const std::byte> container,
                                           std::span<const std::byte> language,
                                           std::uint32_t hash,
                                           std::span<char> output) noexcept {
    if (output.empty()) {
        return false;
    }
    output[0] = '\0';
    Array hashes{}, combinations{};
    if (hash == kNoPlugSource
        || !find_array_at(container, offsetof(schema::generated::SLocalizedStrings, ids), hashes)
        || !find_array_at(language, kLanguageCombinationDescriptor, combinations)
        || hashes.count != combinations.count || hashes.dataOffset > container.size()
        || hashes.count > (container.size() - hashes.dataOffset) / kBankHashStride
        || combinations.dataOffset > language.size()
        || combinations.count > (language.size() - combinations.dataOffset) / kCombinationStride) {
        return false;
    }
    std::size_t ordinal = static_cast<std::size_t>(hashes.count);
    for (std::size_t i = 0; i < hashes.count; ++i) {
        std::uint32_t value{};
        if (read(container, hashes.dataOffset + i * kBankHashStride, value) && value == hash) {
            ordinal = i;
            break;
        }
    }
    if (ordinal == hashes.count) {
        return false;
    }
    const auto combination = combinations.dataOffset + ordinal * kCombinationStride;
    std::int64_t parts{};
    std::size_t first{}, written{};
    if (!read(language, combination + kCombinationCountOffset, parts) || parts < 0
        || parts > kCombinationPartLimit) {
        return false;
    }
    if (parts == 0) {
        return true;
    }
    if (!relative(language, combination + kCombinationPartsField, first)
        || static_cast<std::uint64_t>(parts) > (language.size() - first) / kPartStride) {
        return false;
    }
    for (std::size_t p = 0; p < static_cast<std::size_t>(parts); ++p) {
        const auto part = first + p * kPartStride;
        std::size_t data{};
        std::uint16_t bytes{}, shift{};
        if (!relative(language, part + kPartDataField, data)
            || !read(language, part + kPartByteCountOffset, bytes)
            || !read(language, part + kPartShiftOffset, shift) || bytes > language.size() - data
            || bytes >= output.size() - written) {
            output[0] = '\0';
            return false;
        }
        for (std::size_t i = 0; i < bytes;) {
            const auto c = static_cast<std::uint8_t>(language[data + i]);
            const std::size_t width = c < kUtf8TwoByteLead     ? 1
                                      : c < kUtf8ThreeByteLead ? 2
                                      : c < kUtf8FourByteLead  ? 3
                                                               : 4;
            if (width > bytes - i || c >= kUtf8InvalidLead) {
                output[0] = '\0';
                return false;
            }
            // The shift applies to the last byte of each sequence only.
            for (std::size_t j = 0; j < width; ++j) {
                auto decoded = static_cast<std::uint8_t>(language[data + i + j]);
                if (j + 1 == width) {
                    decoded = static_cast<std::uint8_t>(decoded + shift);
                }
                output[written++] = static_cast<char>(decoded);
            }
            i += width;
        }
    }
    output[written] = '\0';
    return true;
}
} // namespace sunrise::middleware::content::packages::tables::activities
