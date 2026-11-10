#include <algorithm>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "kill_incident.h"

// Field widths are the bit counts the runtime metadata schema of the named class declares.

namespace sunrise::middleware::bap::activity_message::kill_incident {
namespace {

namespace bits = encoding::bits;

/** One field read only to pass it; an optional field is preceded by a presence bit. */
struct SkippedField final {
    std::size_t bits{};
    bool optional{};
};

constexpr std::uint8_t kPresenceBits = 1;
constexpr std::uint8_t kHashBits = 32;
/** Player keys are 64 bits on the wire, as Sensor auth bodies write them. */
constexpr std::uint8_t kPlayerKeyBits = 64;
/** Signed byte fields are written biased by one, so a raw zero is -1. */
constexpr std::int32_t kSignedByteBias = 1;
/** Widths of the plain fields the classes below repeat, in bits. */
constexpr std::uint8_t kFlagBits = 1;
constexpr std::uint8_t kByteBits = 8;
constexpr std::uint8_t kWordBits = 32;
constexpr std::uint8_t kLongWordBits = 64;
constexpr std::uint8_t kRealBits = 32;
/** An uncompressed vector is three reals. */
constexpr std::size_t kVectorBits = 3 * kRealBits;
/** C80809f7b is an array of eight bytes. */
constexpr std::size_t kByteArrayBits = 8 * kByteBits;

/** C80809512 opens C808087e2: a 32-bit word, then C80809514's 3-bit byte and 64-bit word. */
constexpr std::uint8_t kHeaderByteBits = 3;
constexpr std::size_t kHeaderBits = kWordBits + kHeaderByteBits + kLongWordBits;
/** C808087e2 continues with the killing source's equipment slot and ability selector. */
constexpr std::uint8_t kEquipmentSlotBits = 5;
constexpr std::uint8_t kAbilitySelectorBits = 7;
/** The fields between the ability selector and the damage type. Two are 11 bits wide. */
constexpr std::uint8_t kEventNarrowFieldBits = 11;
constexpr SkippedField kEventFields[]{
    {kVectorBits, false},
    {kEventNarrowFieldBits, true},
    {kWordBits, true},
    {kEventNarrowFieldBits, false},
    {kWordBits, false},
    {kWordBits, false},
};
/** C808087e2 then names the final blow's damage type in one biased signed byte. */
constexpr std::uint8_t kDamageTypeBits = 3;
/** C80804c7c holds one whole byte and one 3-bit byte. */
constexpr std::uint8_t kTailNarrowByteBits = 3;
/** The rest of C808087e2, then the fields C80806454 adds after it. */
constexpr SkippedField kEventTail[]{
    {kByteBits + kTailNarrowByteBits, true}, // C80804c7c
    {kFlagBits, false},
    {kByteArrayBits, false}, // three C80809f7b arrays
    {kByteArrayBits, false},
    {kByteArrayBits, false},
};
/** C808087e3 opens each C80806446 actor with three flags, the first of them the combatant flag. */
constexpr std::size_t kActorTrailingFlagBits = 2 * kFlagBits;
/** C808087e3 closes with three signed bytes. */
constexpr std::uint8_t kActorFirstByteBits = 3;
constexpr std::uint8_t kActorSecondByteBits = 2;
constexpr std::uint8_t kActorThirdByteBits = 6;
constexpr std::size_t kActorByteBits =
    kActorFirstByteBits + kActorSecondByteBits + kActorThirdByteBits;
/** Two optional words follow the C808087e3 block, and again both actors. */
constexpr SkippedField kOptionalWordPair[]{{kWordBits, true}, {kWordBits, true}};
/** C80806446 fields after the player key. */
constexpr SkippedField kActorTail[]{
    {kLongWordBits, true},
    {kLongWordBits, true},
    {kWordBits, true},
};

/** C80806444 and C8080644a open with a four-bit element count. */
constexpr std::uint8_t kListCountBits = 4;
/** C80806444 lists up to nine C80806445 records. */
constexpr std::uint64_t kRecordCapacity =
    sizeof(generated::C80806443) / sizeof(generated::C80806445);
/** C8080880e holds a 64-bit and a 32-bit word. */
constexpr std::size_t kRecordWordPairBits = kLongWordBits + kWordBits;
/** Widths of C8080880d's signed bytes: one after its player key and two that close it. */
constexpr std::uint8_t kRecordBodyFirstByteBits = 6;
constexpr std::uint8_t kRecordBodySecondByteBits = 3;
constexpr std::uint8_t kRecordBodyThirdByteBits = 2;
/** C8080880d: a player key, a signed byte, a 64-bit word, C8080880e, a flag, two signed bytes. */
constexpr std::size_t kRecordBodyBits =
    kPlayerKeyBits + kRecordBodyFirstByteBits + kLongWordBits + kRecordWordPairBits + kFlagBits
    + kRecordBodySecondByteBits + kRecordBodyThirdByteBits;
/** C80806445 opens with a signed byte. */
constexpr std::uint8_t kRecordLeadingByteBits = 3;
/** C80806445: a signed byte, C8080880d and a biased byte. */
constexpr std::size_t kRecordBits = kRecordLeadingByteBits + kRecordBodyBits + kByteBits;
/** Two words close the payload after its three label lists. */
constexpr std::size_t kTrailingWordBits = 2 * kWordBits;
static_assert(kRecordCapacity < (1U << kListCountBits)
              && kLabelCapacity < (1U << kListCountBits));

/** Skips fields in wire order; an optional field's value is present only when its bit is set. */
[[nodiscard]] bool skip_fields(bits::Reader& reader,
                               std::span<const SkippedField> fields) noexcept {
    for (const SkippedField& field : fields) {
        std::uint64_t present = 1;
        if ((field.optional && !reader.read(kPresenceBits, present))
            || (present != 0 && !reader.skip(field.bits))) {
            return false;
        }
    }
    return true;
}

/** Reads one optional field; an absent field reads as zero. */
[[nodiscard]] bool
read_optional(bits::Reader& reader, std::uint8_t width, std::uint64_t& value) noexcept {
    std::uint64_t present = 0;
    value = 0;
    return reader.read(kPresenceBits, present) && (present == 0 || reader.read(width, value));
}

/** @return The signed byte a biased raw value encodes. */
[[nodiscard]] std::int8_t unbias(std::uint64_t raw) noexcept {
    return static_cast<std::int8_t>(static_cast<std::int32_t>(raw) - kSignedByteBias);
}

/** Reads one optional biased signed byte, keeping an omitted field apart from an encoded -1. */
[[nodiscard]] bool read_optional_signed(bits::Reader& reader,
                                        std::uint8_t width,
                                        std::optional<std::int8_t>& value) noexcept {
    std::uint64_t present = 0;
    std::uint64_t raw = 0;
    value.reset();
    if (!reader.read(kPresenceBits, present) || (present != 0 && !reader.read(width, raw))) {
        return false;
    }
    if (present != 0) {
        value = unbias(raw);
    }
    return true;
}

/** Skips one counted list of fixed-width elements, refusing a count above its capacity. */
[[nodiscard]] bool
skip_list(bits::Reader& reader, std::uint64_t capacity, std::size_t elementBits) noexcept {
    std::uint64_t count = 0;
    return reader.read(kListCountBits, count) && count <= capacity
           && reader.skip(static_cast<std::size_t>(count) * elementBits);
}

/** Reads one label list; a zero or repeated name is refused. */
[[nodiscard]] bool read_labels(bits::Reader& reader, Labels& output) noexcept {
    std::uint64_t count = 0;
    if (!reader.read(kListCountBits, count) || count > output.values.size()) {
        return false;
    }
    output.count = static_cast<std::size_t>(count);
    for (std::size_t i = 0; i < output.count; ++i) {
        std::uint64_t hash = 0;
        if (!reader.read(kHashBits, hash) || hash == 0) {
            return false;
        }
        output.values[i] = static_cast<std::uint32_t>(hash);
        const auto prior = std::span(output.values).first(i);
        if (std::find(prior.begin(), prior.end(), output.values[i]) != prior.end()) {
            return false;
        }
    }
    return true;
}

/** Reads one C80806446 actor and keeps its combatant flag, class hash and player key. */
[[nodiscard]] bool read_actor(bits::Reader& reader, Actor& output) noexcept {
    std::uint64_t combatant = 0;
    std::uint64_t classHash = 0;
    std::uint64_t playerKey = 0;
    if (!reader.read(kFlagBits, combatant) || !reader.skip(kActorTrailingFlagBits)
        || !read_optional(reader, kHashBits, classHash) || !reader.skip(kActorByteBits)
        || !skip_fields(reader, kOptionalWordPair)
        || !read_optional(reader, kPlayerKeyBits, playerKey) || !skip_fields(reader, kActorTail)) {
        return false;
    }
    output.combatant = combatant != 0;
    output.classHash = static_cast<std::uint32_t>(classHash);
    output.playerKey = playerKey;
    return true;
}

/** Accepts only the zero padding that completes the last byte. */
[[nodiscard]] bool finish_padding(bits::Reader& reader) noexcept {
    const std::size_t paddingBits = reader.remaining_bits();
    std::uint64_t padding = 0;
    return paddingBits < encoding::kBitsPerByte
           && (paddingBits == 0
               || (reader.read(static_cast<std::uint8_t>(paddingBits), padding) && padding == 0));
}

} // namespace

/**
 * Decodes one C80806442 payload: the event block, killer, victim, two optional words, the
 * C80806445 list, a real, three label lists, two words and zero padding.
 * @param input Exact payload bytes.
 * @param output Cleared first. Receives the kill facts.
 * @return True when the whole payload decoded.
 */
bool decode(std::span<const std::byte> input, Payload& output) noexcept {
    output = {};
    bits::Reader reader(input);
    Payload parsed{};
    std::uint64_t damageType = 0;
    if (!reader.skip(kHeaderBits)
        || !read_optional_signed(reader, kEquipmentSlotBits, parsed.equipmentSlot)
        || !read_optional_signed(reader, kAbilitySelectorBits, parsed.abilitySelector)
        || !skip_fields(reader, kEventFields) || !reader.read(kDamageTypeBits, damageType)
        || !skip_fields(reader, kEventTail) || !read_actor(reader, parsed.killer)
        || !read_actor(reader, parsed.victim) || !skip_fields(reader, kOptionalWordPair)
        || !skip_list(reader, kRecordCapacity, kRecordBits) || !reader.skip(kRealBits)
        || !read_labels(reader, parsed.source) || !read_labels(reader, parsed.actor)
        || !read_labels(reader, parsed.target) || !reader.skip(kTrailingWordBits)
        || !finish_padding(reader)) {
        return false;
    }
    parsed.damageType = unbias(damageType);
    output = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::kill_incident
