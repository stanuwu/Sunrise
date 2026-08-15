#include "internal.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {
namespace {

using encoding::bits::Reader;

/** Selection reason uses 4 bits and bias 1. */
constexpr std::uint8_t kReasonWidth = 4;
/** Source and destination activity indices use 12 bits and bias 1. */
constexpr std::uint8_t kActivityIndexWidth = 12;
/** Optional element indices use 9 bits and bias 1. */
constexpr std::uint8_t kElementIndexWidth = 9;
/** All reason, index and skull signed values use wire bias 1. */
constexpr std::int16_t kScalarBias = 1;
/** Identity-like and runtime-nonce fields each take 64 bits when present. */
constexpr std::size_t kSensitiveScalarWidth = 64;
/** The skull count uses 5 bits, so it can declare more entries than storage holds. */
constexpr std::uint8_t kSkullCountWidth = 5;
/** Each skull entry is a 2-bit group followed by a 7-bit value. */
constexpr std::uint8_t kSkullGroupWidth = 2;
constexpr std::uint8_t kSkullValueWidth = 7;
/** The fixed unknown field and its optional peer each take 8 bits. */
constexpr std::size_t kUnknownByteWidth = 8;
/** Fixed package-name elements are signed bytes encoded with bias 128. */
constexpr std::uint8_t kPackageNameWidth = 8;
constexpr std::int16_t kPackageNameBias = 128;
/** Sunrise publishes only lowercase ASCII, digits and underscores in a package name. */
constexpr std::int8_t kLowercaseMinimum = 'a';
constexpr std::int8_t kLowercaseMaximum = 'z';
constexpr std::int8_t kDigitMinimum = '0';
constexpr std::int8_t kDigitMaximum = '9';
constexpr std::int8_t kUnderscore = '_';
/** Zero marks padding after the validated package-name prefix. */
constexpr std::int8_t kPackageNamePadding = 0;
/** A present package name cannot publish an empty prefix. */
constexpr std::uint8_t kEmptyPackageNameLength = 0;
/** The unknown optional hash after the package name takes 32 bits. */
constexpr std::size_t kUnknownHashWidth = 32;
/** The descriptor's fixed boolean takes 1 bit. */
constexpr std::size_t kBooleanWidth = 1;
/** The first unknown tail array uses a 1-bit count and 96-bit entries. */
constexpr std::uint8_t kReferenceCountWidth = 1;
constexpr std::size_t kReferenceEntryWidth = 96;
/** The second unknown tail array uses a 6-bit count and 13-bit entries. */
constexpr std::uint8_t kPairCountWidth = 6;
constexpr std::size_t kPairEntryWidth = 13;

/** @return True when one decoded byte is allowed in a published package name. */
[[nodiscard]] bool is_package_name_character(std::int8_t value) noexcept {
    return (value >= kLowercaseMinimum && value <= kLowercaseMaximum)
           || (value >= kDigitMinimum && value <= kDigitMaximum) || value == kUnderscore;
}

/**
 * Reads the optional element index. The caller output is unchanged on failure.
 * @param reader Bounded MSB-first descriptor reader.
 * @param selection Temporary scalar-only selection output.
 * @return True when the presence marker and the optional biased value are complete.
 */
[[nodiscard]] bool read_element_index(Reader& reader,
                                      ActivityManagerSelection& selection) noexcept {
    bool present = false;
    if (!read_presence(reader, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    std::int16_t value = 0;
    if (!read_i16(reader, kElementIndexWidth, kScalarBias, value)) {
        return false;
    }
    selection.hasElementIndex = true;
    selection.elementIndex = value;
    return true;
}

/**
 * Reads the skull array, keeping what storage holds and walking past the rest. The 5-bit count
 * reaches 31 and storage holds 16, so a bigger count is still a real encoding. Every entry's
 * bits are read either way, so the fields after the array stay aligned.
 * @param reader Bounded MSB-first descriptor reader.
 * @param selection Temporary scalar-only selection output.
 * @return True when the count and every entry's bits are complete.
 */
[[nodiscard]] bool read_skulls(Reader& reader, ActivityManagerSelection& selection) noexcept {
    std::uint64_t encodedCount = 0;
    if (!reader.read(kSkullCountWidth, encodedCount)) {
        return false;
    }
    selection.skullCount = static_cast<std::uint8_t>(
        (std::min)(encodedCount, static_cast<std::uint64_t>(kActivityManagerSkullCapacity)));
    for (std::uint64_t index = 0; index < encodedCount; ++index) {
        ActivityManagerSkullValue value{};
        if (!read_i8(reader, kSkullGroupWidth, kScalarBias, value.group)
            || !read_i8(reader, kSkullValueWidth, kScalarBias, value.value)) {
            return false;
        }
        if (index < selection.skullCount) {
            selection.skulls[index] = value;
        }
    }
    return true;
}

/**
 * Reads the optional fixed package name, ending it at the first byte the text rule rejects.
 * The field is always 40 elements and its tail is padding, so a byte that cannot appear in a
 * name ends the name instead of failing the descriptor and losing the destination choice.
 * @param reader Bounded MSB-first descriptor reader.
 * @param descriptorStart Reader bits left at the descriptor's first bit.
 * @param selection Temporary scalar-only selection output.
 * @return True when the presence marker and all 40 biased bytes are complete.
 */
[[nodiscard]] bool read_package_name(Reader& reader,
                                     std::size_t descriptorStart,
                                     ActivityManagerSelection& selection) noexcept {
    bool present = false;
    if (!read_presence(reader, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    // A forced destination rewrites these bytes in place, so where they sit is kept with them.
    const std::size_t nameStart = descriptorStart - reader.remaining_bits();
    bool ended = false;
    std::uint8_t length = 0;
    for (std::int8_t& character : selection.packageName) {
        std::int8_t decoded = kPackageNamePadding;
        if (!read_i8(reader, kPackageNameWidth, kPackageNameBias, decoded)) {
            return false;
        }
        ended = ended || !is_package_name_character(decoded);
        // Only the accepted prefix is published. The rest of the fixed field stays cleared.
        character = ended ? kPackageNamePadding : decoded;
        length = ended ? length : static_cast<std::uint8_t>(length + 1);
    }
    if (length == kEmptyPackageNameLength) {
        return true;
    }
    selection.hasPackageName = true;
    selection.packageNameLength = length;
    selection.packageNameBitOffset = nameStart;
    return true;
}

/**
 * Reads the safe prefix fields and skips the identity-like scalar and the runtime nonce.
 * @param reader Bounded MSB-first descriptor reader.
 * @param selection Temporary scalar-only selection output.
 * @return True when every prefix field and skull entry is complete.
 */
[[nodiscard]] bool read_prefix(Reader& reader, ActivityManagerSelection& selection) noexcept {
    return read_i8(reader, kReasonWidth, kScalarBias, selection.reason)
           && read_i16(reader, kActivityIndexWidth, kScalarBias, selection.sourceActivityIndex)
           && read_i16(reader, kActivityIndexWidth, kScalarBias, selection.activityIndex)
           && read_element_index(reader, selection) && skip_optional(reader, kSensitiveScalarWidth)
           && skip_optional(reader, kSensitiveScalarWidth) && read_skulls(reader, selection);
}

/**
 * Reads the safe destination fields and skips the unknown fixed and optional scalars.
 * @param reader Bounded MSB-first descriptor reader.
 * @param descriptorStart Reader bits left at the descriptor's first bit.
 * @param selection Temporary scalar-only selection output.
 * @return True when every destination field is complete.
 */
[[nodiscard]] bool read_destination(Reader& reader,
                                    std::size_t descriptorStart,
                                    ActivityManagerSelection& selection) noexcept {
    return reader.skip(kUnknownByteWidth) && skip_optional(reader, kUnknownByteWidth)
           && read_hash(reader, selection.hasArrivalBubbleHash, selection.arrivalBubbleHash)
           && read_hash(reader, selection.hasSpawnSetHash, selection.spawnSetHash)
           && read_package_name(reader, descriptorStart, selection)
           && skip_optional(reader, kUnknownHashWidth) && reader.skip(kBooleanWidth);
}

/**
 * Skips both unknown descriptor-tail arrays, whatever their counts say. Nothing here is
 * published, so the bits only need to be present. The 6-bit and 1-bit counts run past the
 * schema storage, so a count that can be read is walked, not refused.
 * @param reader Bounded MSB-first descriptor reader.
 * @return True when both presence markers, counts and payloads are complete.
 */
[[nodiscard]] bool skip_tail(Reader& reader) noexcept {
    bool hasReferences = false;
    std::uint64_t referenceCount = 0;
    if (!read_presence(reader, hasReferences)
        || (hasReferences
            && (!reader.read(kReferenceCountWidth, referenceCount)
                || !reader.skip(static_cast<std::size_t>(referenceCount)
                                * kReferenceEntryWidth)))) {
        return false;
    }

    bool hasPairs = false;
    std::uint64_t pairCount = 0;
    if (!read_presence(reader, hasPairs)) {
        return false;
    }
    if (!hasPairs) {
        return true;
    }
    return reader.read(kPairCountWidth, pairCount)
           && reader.skip(static_cast<std::size_t>(pairCount) * kPairEntryWidth);
}

} // namespace

/** Reads one complete descriptor into safe scalar-only output. */
bool parse(Reader& reader, ActivityManagerSelection& selection) noexcept {
    const std::size_t descriptorStart = reader.remaining_bits();
    return read_prefix(reader, selection) && read_destination(reader, descriptorStart, selection)
           && skip_tail(reader);
}

} // namespace
  // sunrise::middleware::bap::activity_host_manager::request::selection
