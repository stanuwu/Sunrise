#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_writer.h"

namespace sunrise::middleware::bap::activity_message::scoreboard_record {

/**
 * Schema classes of the scoreboard record and of the auth-body carrier it rides in.
 * Every width and count below is one of those two classes' own field declarations.
 */
inline constexpr std::uint32_t kRecordClassId = 0x808099F9U;
inline constexpr std::uint32_t kComponentClassId = 0x808099F1U;
/** The wire slot type the global activity group publishes this component at. */
inline constexpr std::uint8_t kSlotType = 16;

/** Presence bits the record's root writes, one per declared field. */
inline constexpr std::size_t kRootFieldCount = 7;
inline constexpr std::uint16_t kElementsFieldIndex = 0;
inline constexpr std::uint16_t kTeamListFieldIndex = 1;

/** The element array declares its length in five bits and reserves sixteen seats. */
inline constexpr std::uint8_t kElementCountWidth = 5;
inline constexpr std::size_t kElementCount = 16;

/** One element is a 64-bit key behind its presence bit, with four declared fields between. */
inline constexpr std::uint8_t kElementKeyWidth = 64;
inline constexpr std::size_t kElementMiddleFieldCount = 4;
inline constexpr std::size_t kEmptyElementBits = 1 + kElementMiddleFieldCount + 1;
inline constexpr std::size_t kKeyedElementBits = kEmptyElementBits + kElementKeyWidth;

inline constexpr std::size_t kElementsSubtreeBits = kElementCountWidth;

/** The team list declares its length in four bits and reserves twelve teams. */
inline constexpr std::uint8_t kTeamCountWidth = 4;
inline constexpr std::size_t kTeamCount = 12;
/** A neutral team writes three absent-field bits and one false mandatory boolean. */
inline constexpr std::size_t kNeutralTeamEntryBits = 4;
[[nodiscard]] constexpr std::size_t team_list_subtree_bits(std::uint8_t teamCount) noexcept {
    return kTeamCountWidth + std::size_t{teamCount} * kNeutralTeamEntryBits;
}

struct Element final {

    std::uint64_t key{};
    bool live{};
    bool hasKey{};
};

struct Record final {
    std::array<Element, kElementCount> elements{};
    std::uint8_t elementCount{};
    bool hasTeamList{};
    std::uint8_t teamCount{};
};

[[nodiscard]] constexpr std::size_t record_body_bits(const Record& record) noexcept {
    std::size_t bits = kRootFieldCount + kElementCountWidth;
    // Only the seats the count field declares reach the wire; the client stops there.
    const std::size_t seats =
        record.elementCount < kElementCount ? record.elementCount : kElementCount;
    for (std::size_t index = 0; index < seats; ++index) {
        bits += record.elements[index].hasKey ? kKeyedElementBits : kEmptyElementBits;
    }
    return bits + (record.hasTeamList ? team_list_subtree_bits(record.teamCount) : 0);
}

inline constexpr std::size_t kMinimumRecordBits = kRootFieldCount + kElementsSubtreeBits;
/** Pins the shortest body a reader must still accept, which is the root and an empty array. */
static_assert(kMinimumRecordBits == 12);

[[nodiscard]] bool write_record_body(encoding::bits::Writer& writer, const Record& record) noexcept;

[[nodiscard]] bool valid(const Record& record) noexcept;

} // namespace sunrise::middleware::bap::activity_message::scoreboard_record
