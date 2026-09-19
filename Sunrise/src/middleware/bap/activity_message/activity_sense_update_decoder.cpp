#include <bit>
#include <limits>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "activity_sense_update_reader.h"
#include "sense_update.h"

namespace sunrise::middleware::bap::activity_message::sense_update {
namespace {
using namespace decoding;

/** Field widths of the object header ClientRef, in bits. */
constexpr std::uint8_t kKeyWidth = 32, kTypeWidth = 7, kIndexWidth = 16;
/** The slot type and index are sent unsigned, biased so their absent value is zero. */
constexpr std::uint32_t kTypeBias = 1, kIndexBias = 32768;
/** Schema tags the sense-update objects and their nested arrays declare. */
constexpr std::uint32_t kDevice = 0x80804F47U, kScene = 0x8080626AU;
constexpr std::uint32_t kSquad = 0x80807ECCU, kObjective = 0x80807F04U;
constexpr std::uint32_t kOccupancy = 0x80809531U;
constexpr std::uint32_t kAttachment = 0x8080954AU;
constexpr std::uint32_t kGhostLink = 0x80804D3EU;
constexpr std::uint32_t kObject = 0x8080992EU;
constexpr std::uint32_t kObjectSpawnMask = 0x80809E1BU, kObjectReplies = 0x80809AEAU;
constexpr std::uint32_t kSceneEvents = 0x808094DFU, kSceneList = 0x808094E1U;
constexpr std::uint32_t kSquadCounts = 0x80809491U, kSquadList = 0x80807ECFU;
constexpr std::uint32_t kSquadReals = 0x80807ECDU;
constexpr std::uint32_t kObjectiveBlock = 0x80807F07U, kObjectiveTasks = 0x80807F08U;
constexpr std::uint32_t kCombatant = 0x80807DA2U, kCombatantAtoms = 0x80807F6EU;
constexpr std::uint32_t kCombatantKeyed = 0x80807DA3U, kCombatantLanes = 0x80807DA4U;
constexpr std::uint32_t kEngagement = 0x808094F0U, kEngagementList = 0x808094F8U;
/** Type-13 participation root and its nested blocks, in body order. */
constexpr std::uint32_t kPlayer = 0x80804F2FU, kPlayerState = 0x808094E4U;
constexpr std::uint32_t kPlayerScalars = 0x80804F39U, kPlayerTail = 0x80804F35U;
constexpr std::uint32_t kPlayerIdentity = 0x808094DDU, kPlayerKeys = 0x808094DFU;
constexpr std::uint32_t kPlayerPairs = 0x808094E3U, kPlayerFlags = 0x80804F31U;
constexpr std::uint32_t kPlayerOptional = 0x80804F34U;
/** Type-20 damage monitor root: health, shield, echoed revision. */
constexpr std::uint32_t kDamage = 0x80809562U;
/** Registered object replies: the interaction latch and the ownership key. */
constexpr std::uint32_t kInteractionReply = 0x80804FB7U, kOwnershipReply = 0x80809ACCU;
/** Counted lists inside the participation body. */
constexpr std::uint64_t kPlayerScalarCountMaximum = 4, kPlayerKeyCountMaximum = 32;
/** Quantization ceilings the combatant reals carry in their own descriptors. */
constexpr std::uint32_t kSpatialMaximumBits = 0x45000000U, kUnitMaximumBits = 0x3F800000U;

enum class NativeStatus : std::uint8_t { complete, unsupported, unsafeCount, malformed };

/** @param width Field width in bits. @return Low-bit mask of that width. */
[[nodiscard]] constexpr std::uint64_t mask(std::uint8_t width) noexcept {
    return width == 64 ? (std::numeric_limits<std::uint64_t>::max)()
                       : (std::uint64_t{1} << width) - 1;
}
[[nodiscard]] std::int64_t
signed_value(std::uint64_t raw, std::int32_t bias, std::uint8_t storageWidth) noexcept {
    const std::uint64_t decoded =
        (raw - static_cast<std::uint64_t>(static_cast<std::int64_t>(bias))) & mask(storageWidth);
    if (storageWidth == 64) {
        return static_cast<std::int64_t>(decoded);
    }
    const std::uint64_t sign = std::uint64_t{1} << (storageWidth - 1);
    return static_cast<std::int64_t>((decoded ^ sign) - sign);
}

/** Collects decoded values into the packet, dropping them once the object's room runs out. */
class Values final {
public:
    Values(DecodedPacket& packet, DecodedObject* object) noexcept
        : packet_(packet), object_(object) {}
    /** Retains one field value, marking truncation when the packet's storage is full. */
    void put(std::uint32_t schema,
             std::uint16_t ordinal,
             std::uint32_t occurrence,
             std::uint32_t at,
             std::uint8_t width,
             ValueKind kind,
             std::uint64_t raw,
             std::int64_t signedRaw,
             float real,
             bool present) noexcept {
        if (object_ == nullptr) {
            return;
        }
        if (packet_.valueCount == packet_.values.size()) {
            packet_.valuesTruncated = true;
            return;
        }
        DecodedValue& value = packet_.values[packet_.valueCount++];
        value.schemaRow = schema;
        value.fieldRow = ordinal;
        value.fieldOrdinal = ordinal;
        value.occurrence = occurrence;
        value.bitOffset = at;
        value.width = width;
        value.kind = kind;
        value.unsignedValue = raw;
        value.signedValue = signedRaw;
        value.realValue = real;
        value.present = present;
    }

private:
    DecodedPacket& packet_;
    DecodedObject* object_{};
};

/**
 * Reads one optional or required unsigned field and records it.
 * @param optional True when a presence bit precedes the value.
 * @return True when the presence bit and any value were complete.
 */
[[nodiscard]] bool read_unsigned(Reader& reader,
                                 Values& values,
                                 std::uint32_t schema,
                                 std::uint16_t ordinal,
                                 std::uint8_t width,
                                 bool optional,
                                 std::uint32_t occurrence = 0) noexcept {
    const auto at = static_cast<std::uint32_t>(reader.position());
    bool exists = true;
    if (!present(reader, optional, exists)) {
        return false;
    }
    std::uint64_t raw = 0;
    if (exists && !reader.read(width, raw)) {
        return false;
    }
    values.put(schema,
               ordinal,
               occurrence,
               at,
               exists ? width : 0,
               ValueKind::unsignedInteger,
               raw,
               0,
               0.0F,
               exists);
    return true;
}
/** Removes wire bias and sign-extends to the host width after reading a complete selected field. */
[[nodiscard]] bool read_signed(Reader& reader,
                               Values& values,
                               std::uint32_t schema,
                               std::uint16_t ordinal,
                               std::uint8_t wireWidth,
                               std::uint8_t storageWidth,
                               std::int32_t bias,
                               bool optional,
                               std::uint32_t occurrence = 0) noexcept {
    const auto at = static_cast<std::uint32_t>(reader.position());
    bool exists = true;
    if (!present(reader, optional, exists)) {
        return false;
    }
    std::uint64_t raw = 0;
    if (exists && !reader.read(wireWidth, raw)) {
        return false;
    }
    const std::int64_t decoded = exists ? signed_value(raw, bias, storageWidth) : 0;
    values.put(schema,
               ordinal,
               occurrence,
               at,
               exists ? wireWidth : 0,
               ValueKind::signedInteger,
               static_cast<std::uint64_t>(decoded),
               decoded,
               0.0F,
               exists);
    return true;
}
/** Reads one boolean field into the value table. */
[[nodiscard]] bool read_bool(Reader& reader,
                             Values& values,
                             std::uint32_t schema,
                             std::uint16_t ordinal,
                             bool optional,
                             std::uint32_t occurrence = 0) noexcept {
    const auto at = static_cast<std::uint32_t>(reader.position());
    bool exists = true;
    if (!present(reader, optional, exists)) {
        return false;
    }
    std::uint64_t raw = 0;
    if (exists && !reader.read(1, raw)) {
        return false;
    }
    values.put(
        schema, ordinal, occurrence, at, exists ? 1 : 0, ValueKind::boolean, raw, 0, 0.0F, exists);
    return true;
}
/** Quantized endpoints stay exact; width 32 carries raw float bits. */
[[nodiscard]] float
real_value(std::uint64_t raw, std::uint8_t width, std::uint32_t maximumBits) noexcept {
    if (width == 32) {
        return std::bit_cast<float>(static_cast<std::uint32_t>(raw));
    }
    const float maximum = std::bit_cast<float>(maximumBits);
    const std::uint64_t levels = std::uint64_t{1} << width;
    if (raw == 0) {
        return 0.0F;
    }
    if (raw == levels - 1) {
        return maximum;
    }
    const float step = maximum / static_cast<float>(levels - 2);
    return static_cast<float>(raw - 1) * step + step * 0.5F;
}
/** Records a real only when its presence bit and selected value fit. */
[[nodiscard]] bool read_real(Reader& reader,
                             Values& values,
                             std::uint32_t schema,
                             std::uint16_t ordinal,
                             std::uint8_t width,
                             bool optional,
                             std::uint32_t occurrence = 0,
                             std::uint32_t maximumBits = 0) noexcept {
    const auto at = static_cast<std::uint32_t>(reader.position());
    bool exists = true;
    if (!present(reader, optional, exists)) {
        return false;
    }
    std::uint64_t raw = 0;
    if (exists && !reader.read(width, raw)) {
        return false;
    }
    values.put(schema,
               ordinal,
               occurrence,
               at,
               exists ? width : 0,
               ValueKind::real32,
               raw,
               0,
               exists ? real_value(raw, width, maximumBits) : 0.0F,
               exists);
    return true;
}

/** Decodes three device channels within the object's bit budget. */
[[nodiscard]] NativeStatus decode_device(Reader& reader, Values& values) noexcept {
    for (std::uint16_t channel = 0; channel < 3; ++channel) {
        if (!read_real(reader, values, kDevice, channel * 2, 32, true)
            || !read_signed(reader,
                            values,
                            kDevice,
                            channel * 2 + 1,
                            32,
                            32,
                            (std::numeric_limits<std::int32_t>::min)(),
                            true)) {
            return NativeStatus::malformed;
        }
    }
    return NativeStatus::complete;
}
/** Decodes the occupancy flags and signed values within the object's bit budget. */
[[nodiscard]] NativeStatus decode_occupancy(Reader& reader, Values& values) noexcept {
    return read_bool(reader, values, kOccupancy, 0, false)
                   && read_bool(reader, values, kOccupancy, 1, false)
                   && read_signed(reader,
                                  values,
                                  kOccupancy,
                                  2,
                                  32,
                                  32,
                                  (std::numeric_limits<std::int32_t>::min)(),
                                  false)
                   && read_signed(reader,
                                  values,
                                  kOccupancy,
                                  3,
                                  32,
                                  32,
                                  (std::numeric_limits<std::int32_t>::min)(),
                                  false)
               ? NativeStatus::complete
               : NativeStatus::malformed;
}
/** Decodes a scene and refuses event counts above the native capacity. */
[[nodiscard]] NativeStatus decode_scene(Reader& reader, Values& values) noexcept {
    if (!read_signed(
            reader, values, kScene, 0, 32, 32, (std::numeric_limits<std::int32_t>::min)(), false)
        || !read_bool(reader, values, kScene, 1, false)
        || !read_signed(reader, values, kScene, 2, 31, 32, 0, true)
        || !read_signed(reader, values, kScene, 3, 2, 8, 1, false)) {
        return NativeStatus::malformed;
    }
    std::uint64_t count = 0;
    const auto at = static_cast<std::uint32_t>(reader.position());
    if (!reader.read(6, count)) {
        return NativeStatus::malformed;
    }
    if (count > 32) {
        return NativeStatus::unsafeCount;
    }
    values.put(kSceneList, 0, 0, at, 6, ValueKind::unsignedInteger, count, 0, 0.0F, true);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!read_unsigned(reader, values, kSceneEvents, 0, 32, false, index)) {
            return NativeStatus::malformed;
        }
    }
    return NativeStatus::complete;
}
/** Decodes squad fields and bounded member lists within the object's bit budget. */
[[nodiscard]] NativeStatus decode_squad(Reader& reader, Values& values) noexcept {
    if (!read_signed(reader, values, kSquad, 0, 31, 32, 0, true)
        || !read_signed(reader, values, kSquad, 1, 31, 32, 0, true)
        || !read_signed(reader, values, kSquad, 2, 31, 32, 0, true)
        || !read_signed(reader, values, kSquad, 3, 6, 32, 0, true)
        || !read_real(reader, values, kSquad, 4, 7, true, 0, 1157562368U)
        || !read_signed(reader, values, kSquad, 5, 31, 32, 0, true)
        || !read_signed(reader, values, kSquad, 6, 2, 8, 1, false)
        || !read_signed(reader, values, kSquad, 7, 3, 8, 1, false)
        || !read_bool(reader, values, kSquad, 8, false)
        || !read_bool(reader, values, kSquad, 9, false)
        || !read_bool(reader, values, kSquad, 10, false)) {
        return NativeStatus::malformed;
    }
    bool exists = false;
    if (!present(reader, true, exists)) {
        return NativeStatus::malformed;
    }
    if (exists) {
        std::uint64_t count = 0;
        const auto at = static_cast<std::uint32_t>(reader.position());
        if (!reader.read(4, count)) {
            return NativeStatus::malformed;
        }
        if (count > 8) {
            return NativeStatus::unsafeCount;
        }
        values.put(kSquadList, 0, 0, at, 4, ValueKind::unsignedInteger, count, 0, 0.0F, true);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!read_signed(reader,
                             values,
                             kSquadCounts,
                             0,
                             32,
                             32,
                             (std::numeric_limits<std::int32_t>::min)(),
                             false,
                             index)) {
                return NativeStatus::malformed;
            }
        }
    }
    if (!present(reader, true, exists)) {
        return NativeStatus::malformed;
    }
    if (exists) {
        for (std::uint32_t index = 0; index < 24; ++index) {
            if (!read_real(reader, values, kSquadReals, 0, 7, true, index, 1157562368U)) {
                return NativeStatus::malformed;
            }
        }
    }
    return NativeStatus::complete;
}
/** Decodes the optional objective blocks and their fixed task arrays. */
[[nodiscard]] NativeStatus decode_objective(Reader& reader, Values& values) noexcept {
    bool exists = false;
    if (!present(reader, true, exists)) {
        return NativeStatus::malformed;
    }
    if (exists) {
        for (std::uint32_t block = 0; block < 24; ++block) {
            if (!read_signed(reader, values, kObjectiveBlock, 0, 7, 8, 0, true, block)
                || !read_unsigned(reader, values, kObjectiveBlock, 1, 1, false, block)) {
                return NativeStatus::malformed;
            }
            bool tasks = false;
            if (!present(reader, true, tasks)) {
                return NativeStatus::malformed;
            }
            if (!tasks) {
                continue;
            }
            for (std::uint32_t task = 0; task < 24; ++task) {
                if (!read_signed(
                        reader, values, kObjectiveTasks, 0, 7, 8, 0, true, block * 24 + task)) {
                    return NativeStatus::malformed;
                }
            }
        }
    }
    return read_signed(reader, values, kObjective, 1, 31, 32, 0, true)
                   && read_unsigned(reader, values, kObjective, 2, 32, true)
               ? NativeStatus::complete
               : NativeStatus::malformed;
}
/** Decodes an object and accepts only the registered interaction and ownership replies. */
[[nodiscard]] NativeStatus decode_object(Reader& reader, Values& values) noexcept {
    if (!read_signed(
            reader, values, kObject, 0, 32, 32, (std::numeric_limits<std::int32_t>::min)(), false)
        || !read_bool(reader, values, kObject, 1, false)
        || !read_bool(reader, values, kObject, 2, false)
        || !read_signed(reader,
                        values,
                        kObject,
                        3,
                        32,
                        32,
                        (std::numeric_limits<std::int32_t>::min)(),
                        false)) {
        return NativeStatus::malformed;
    }
    for (std::uint32_t index = 0; index < 2; ++index) {
        if (!read_unsigned(reader, values, kObjectSpawnMask, 0, 32, false, index)) {
            return NativeStatus::malformed;
        }
    }
    std::uint64_t count = 0;
    const auto at = static_cast<std::uint32_t>(reader.position());
    if (!reader.read(2, count)) {
        return NativeStatus::malformed;
    }
    values.put(kObjectReplies, 0, 0, at, 2, ValueKind::unsignedInteger, count, 0, 0.0F, true);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint64_t exists = 0, schema = 0;
        if (!reader.read(1, exists)) {
            return NativeStatus::malformed;
        }
        if (!exists) {
            continue;
        }
        if (!reader.read(32, schema)) {
            return NativeStatus::malformed;
        }
        if (schema == kInteractionReply) {
            // The interaction latch and the revision that set it.
            if (!read_bool(reader, values, kInteractionReply, 0, false, index)
                || !read_signed(reader,
                                values,
                                kInteractionReply,
                                1,
                                32,
                                32,
                                (std::numeric_limits<std::int32_t>::min)(),
                                false,
                                index)) {
                return NativeStatus::malformed;
            }
        } else if (schema == kOwnershipReply) {
            // The owner key is always written, zero when nobody holds the object.
            if (!read_bool(reader, values, kOwnershipReply, 0, false, index)
                || !read_unsigned(reader, values, kOwnershipReply, 1, 64, false, index)) {
                return NativeStatus::malformed;
            }
        } else {
            return NativeStatus::unsupported;
        }
    }
    return NativeStatus::complete;
}
/** Decodes combatant generations, program state, keyed lanes and root flags within the budget. */
[[nodiscard]] NativeStatus decode_combatant(Reader& reader, Values& values) noexcept {
    if (!read_signed(reader, values, kCombatant, 0, 31, 32, 0, true)
        || !read_real(reader, values, kCombatant, 1, 9, true, 0, kSpatialMaximumBits)
        || !read_signed(reader, values, kCombatant, 2, 31, 32, 0, true)) {
        return NativeStatus::malformed;
    }
    bool exists = false;
    if (!present(reader, true, exists)) {
        return NativeStatus::malformed;
    }
    if (exists
        && (!read_signed(reader, values, kCombatantAtoms, 0, 6, 32, 0, true)
            || !read_signed(reader, values, kCombatantAtoms, 1, 31, 32, 0, true)
            || !read_signed(reader, values, kCombatantAtoms, 2, 31, 32, 0, true)
            || !read_bool(reader, values, kCombatantAtoms, 3, false))) {
        return NativeStatus::malformed;
    }
    if (!present(reader, true, exists)) {
        return NativeStatus::malformed;
    }
    if (exists) {
        bool lanes = false;
        if (!present(reader, true, lanes)) {
            return NativeStatus::malformed;
        }
        for (std::uint32_t index = 0; lanes && index < 8; ++index) {
            if (!read_signed(reader, values, kCombatantLanes, 0, 31, 32, 0, true, index)) {
                return NativeStatus::malformed;
            }
        }
        if (!read_unsigned(reader, values, kCombatantKeyed, 1, 32, true)) {
            return NativeStatus::malformed;
        }
    }
    if (!read_signed(reader, values, kCombatant, 5, 31, 32, 0, true)
        || !read_signed(reader, values, kCombatant, 6, 2, 8, 1, false)
        || !read_signed(reader, values, kCombatant, 7, 31, 32, 0, true)
        || !read_real(reader, values, kCombatant, 8, 7, true, 0, kUnitMaximumBits)
        || !read_real(reader, values, kCombatant, 9, 7, true, 0, kUnitMaximumBits)
        || !read_bool(reader, values, kCombatant, 10, false)
        || !read_bool(reader, values, kCombatant, 11, false)) {
        return NativeStatus::malformed;
    }
    return NativeStatus::complete;
}
/**
 * The observed engagement list is empty. Nonempty custom35 elements remain unsupported until
 * their native codec is established; a storage size is not a wire width.
 */
[[nodiscard]] NativeStatus decode_engagement(Reader& reader, Values& values) noexcept {
    constexpr std::uint64_t capacity = 16;
    std::uint64_t count = 0;
    const auto at = static_cast<std::uint32_t>(reader.position());
    if (!reader.read(5, count)) {
        return NativeStatus::malformed;
    }
    if (count > capacity) {
        return NativeStatus::unsafeCount;
    }
    if (count != 0) {
        return NativeStatus::unsupported;
    }
    values.put(kEngagementList, 0, 0, at, 5, ValueKind::unsignedInteger, count, 0, 0.0F, true);
    return read_signed(reader, values, kEngagement, 1, 16, 16, 32768, false)
               ? NativeStatus::complete
               : NativeStatus::malformed;
}

/** Build86657's attachment Sense has three required s32 counters and one bool. */
[[nodiscard]] NativeStatus decode_attachment(Reader& reader, Values& values) noexcept {
    for (std::uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
        if (!read_signed(reader,
                         values,
                         kAttachment,
                         ordinal,
                         32,
                         32,
                         (std::numeric_limits<std::int32_t>::min)(),
                         false)) {
            return NativeStatus::malformed;
        }
    }
    return read_bool(reader, values, kAttachment, 3, false) ? NativeStatus::complete
                                                            : NativeStatus::malformed;
}

/**
 * Decodes the type-13 participation body. The whole body is read before its Ghost flag counts,
 * because a player mid-load publishes a body with the flag but without a settled actor.
 */
[[nodiscard]] NativeStatus decode_player(Reader& reader, Values& values) noexcept {
    // The participation field uses a signed 32-bit midpoint bias.
    constexpr auto signedBias = (std::numeric_limits<std::int32_t>::min)();
    if (!read_signed(reader, values, kPlayerState, 0, 32, 32, signedBias, true)
        || !read_unsigned(reader, values, kPlayerState, 1, 32, true)) {
        return NativeStatus::malformed;
    }
    for (std::uint16_t ordinal = 2; ordinal <= 5; ++ordinal) {
        if (!read_bool(reader, values, kPlayerState, ordinal, false)) {
            return NativeStatus::malformed;
        }
    }
    if (!read_signed(reader, values, kPlayerState, 6, 3, 8, 1, false)
        || !read_signed(reader, values, kPlayerState, 7, 2, 8, 1, false)) {
        return NativeStatus::malformed;
    }
    std::uint64_t count = 0;
    if (!reader.read(3, count)) {
        return NativeStatus::malformed;
    }
    if (count > kPlayerScalarCountMaximum) {
        return NativeStatus::unsafeCount;
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!read_unsigned(reader, values, kPlayerScalars, 0, 32, false, index)
            || !read_real(reader, values, kPlayerScalars, 1, 32, false, index)) {
            return NativeStatus::malformed;
        }
    }
    if (!read_real(reader, values, kPlayerTail, 1, 32, false)
        || !read_signed(reader, values, kPlayerTail, 2, 5, 32, 1, false)
        || !read_signed(reader, values, kPlayerTail, 3, 32, 32, signedBias, true)) {
        return NativeStatus::malformed;
    }
    for (std::uint16_t ordinal = 4; ordinal <= 6; ++ordinal) {
        if (!read_bool(reader, values, kPlayerTail, ordinal, false)) {
            return NativeStatus::malformed;
        }
    }
    if (!read_unsigned(reader, values, kPlayerIdentity, 0, 64, true)
        || !read_unsigned(reader, values, kPlayerIdentity, 1, 5, false)
        || !read_signed(reader, values, kPlayerIdentity, 2, 6, 8, 3, false)) {
        return NativeStatus::malformed;
    }
    if (!reader.read(6, count)) {
        return NativeStatus::malformed;
    }
    if (count > kPlayerKeyCountMaximum) {
        return NativeStatus::unsafeCount;
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!read_unsigned(reader, values, kPlayerKeys, 0, 32, false, index)) {
            return NativeStatus::malformed;
        }
    }
    if (!reader.read(6, count)) {
        return NativeStatus::malformed;
    }
    if (count > kPlayerKeyCountMaximum) {
        return NativeStatus::unsafeCount;
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!read_unsigned(reader, values, kPlayerPairs, 0, 32, true, index)
            || !read_unsigned(reader, values, kPlayerPairs, 1, 64, true, index)) {
            return NativeStatus::malformed;
        }
    }
    if (!read_bool(reader, values, kPlayerFlags, 0, false)
        || !read_unsigned(reader, values, kPlayerFlags, 1, 4, false)
        || !read_unsigned(reader, values, kPlayerFlags, 2, 16, true)
        || !read_unsigned(reader, values, kPlayerFlags, 3, 16, true)) {
        return NativeStatus::malformed;
    }
    bool exists = false;
    if (!present(reader, true, exists)) {
        return NativeStatus::malformed;
    }
    if (exists && !read_unsigned(reader, values, kPlayerOptional, 0, 32, false)) {
        return NativeStatus::malformed;
    }
    return read_bool(reader, values, kPlayerFlags, 5, false)
                   && read_signed(reader, values, kPlayerFlags, 6, 8, 8, 128, false)
                   && read_signed(reader, values, kPlayer, 4, 32, 32, signedBias, false)
               ? NativeStatus::complete
               : NativeStatus::malformed;
}

/** Decodes one root body of the given schema; the status names how far it got. */
[[nodiscard]] NativeStatus
decode_body(std::uint32_t schema, Reader& reader, Values& values) noexcept {
    std::uint64_t root = 0;
    if (!reader.read(1, root)) {
        return NativeStatus::malformed;
    }
    if (root == 0) {
        return NativeStatus::complete;
    }
    switch (schema) {
    case kPlayer:
        return decode_player(reader, values);
    case kDamage:
        // Three mandatory fields: health, shield, then the echoed Auth revision.
        return read_real(reader, values, schema, 0, 32, false)
                       && read_real(reader, values, schema, 1, 32, false)
                       && read_signed(reader,
                                      values,
                                      schema,
                                      2,
                                      32,
                                      32,
                                      (std::numeric_limits<std::int32_t>::min)(),
                                      false)
                   ? NativeStatus::complete
                   : NativeStatus::malformed;
    case kGhostLink:
        // Three mandatory fields: active, elapsed over duration, accepted Auth generation.
        return read_bool(reader, values, kGhostLink, 0, false)
                       && read_real(reader, values, kGhostLink, 1, 32, false)
                       && read_signed(reader,
                                      values,
                                      kGhostLink,
                                      2,
                                      32,
                                      32,
                                      (std::numeric_limits<std::int32_t>::min)(),
                                      false)
                   ? NativeStatus::complete
                   : NativeStatus::malformed;
    case kDevice:
        return decode_device(reader, values);
    case kScene:
        return decode_scene(reader, values);
    case kSquad:
        return decode_squad(reader, values);
    case kObjective:
        return decode_objective(reader, values);
    case kOccupancy:
        return decode_occupancy(reader, values);
    case kAttachment:
        return decode_attachment(reader, values);
    case kObject:
        return decode_object(reader, values);
    case kCombatant:
        return decode_combatant(reader, values);
    case kEngagement:
        return decode_engagement(reader, values);
    default:
        return NativeStatus::unsupported;
    }
}
[[nodiscard]] bool skip_group(Reader& reader) noexcept {
    if (reader.left() == 0 || !reader.skip(reader.left() - 1)) {
        return false;
    }
    std::uint64_t end = 0;
    return reader.read(1, end) && end == 0 && reader.left() == 0;
}
void finish(SenseUpdate& update,
            bits::Reader& reader,
            std::size_t total,
            DecodeStatus status,
            std::size_t& consumed) noexcept {
    update.decoded.status = status;
    update.decoded.bitsConsumed = total - reader.remaining_bits();
    update.decoded.bitsRemaining = reader.remaining_bits();
    consumed = update.decoded.bitsConsumed;
}
} // namespace

/** Decodes epoch and sensor blocks with per-group budgets and exact zero padding. */
bool decode_sense_update(std::span<const std::byte> input,
                         const Resolver& resolver,
                         SenseUpdate& update,
                         std::size_t& consumed) noexcept {
    update = {};
    consumed = 0;
    if (input.size() > kOuterByteCapacity) {
        return false;
    }
    const std::size_t total = input.size() * encoding::kBitsPerByte;
    bits::Reader reader(input);
    std::uint64_t literal = 0, root = 0;
    if (!reader.read(kEpochFieldWidth, update.epoch.first)
        || !reader.read(kEpochFieldWidth, update.epoch.second) || !reader.read(1, literal)
        || literal != 0 || !reader.read(1, root)) {
        finish(update, reader, total, DecodeStatus::malformed, consumed);
        return false;
    }
    update.tailBits = static_cast<std::uint32_t>(reader.remaining_bits() + 1);
    if (root != 0) {
        Reader rootBody(reader, reader.remaining_bits(), total);
        if (!consume_root_sense(rootBody)) {
            finish(update, reader, total, DecodeStatus::malformed, consumed);
            return false;
        }
    }
    bool partial = false;
    for (;;) {
        std::uint64_t groupPresent = 0;
        if (!reader.read(1, groupPresent)) {
            finish(update, reader, total, DecodeStatus::malformed, consumed);
            return false;
        }
        if (groupPresent == 0) {
            break;
        }
        std::uint64_t groupKey = 0, groupBits = 0;
        if (!reader.read(32, groupKey) || !reader.read(32, groupBits)
            || groupBits > kGroupByteCapacity * 8 || groupBits > reader.remaining_bits()) {
            finish(update, reader, total, DecodeStatus::malformed, consumed);
            return false;
        }
        ++update.decoded.groupsSeen;
        GroupTarget group{};
        const TargetStatus groupStatus =
            resolver.resolveGroup == nullptr
                ? TargetStatus::targetUnavailable
                : resolver.resolveGroup(
                      resolver.context, static_cast<std::uint32_t>(groupKey), group);
        Reader body(reader, static_cast<std::size_t>(groupBits), total);
        if (groupStatus != TargetStatus::resolved) {
            if (!skip_group(body)) {
                finish(update, reader, total, DecodeStatus::malformed, consumed);
                return false;
            }
            ++update.decoded.groupsSkipped;
            partial = true;
            continue;
        }
        bool skipped = false, hasObject = false;
        while (!skipped) {
            std::uint64_t objectPresent = 0;
            if (!body.read(1, objectPresent)) {
                finish(update, reader, total, DecodeStatus::malformed, consumed);
                return false;
            }
            if (objectPresent == 0) {
                if (!hasObject || body.left() != 0) {
                    finish(update, reader, total, DecodeStatus::malformed, consumed);
                    return false;
                }
                ++update.decoded.groupsDecoded;
                break;
            }
            hasObject = true;
            ++update.decoded.objectsSeen;
            std::uint64_t objectKey = 0, typeRaw = 0, indexRaw = 0;
            if (!body.read(kKeyWidth, objectKey) || objectKey != groupKey
                || !body.read(kTypeWidth, typeRaw) || typeRaw < kTypeBias
                || !body.read(kIndexWidth, indexRaw) || indexRaw < kIndexBias) {
                finish(update, reader, total, DecodeStatus::malformed, consumed);
                return false;
            }
            const auto type = static_cast<std::uint8_t>(typeRaw - kTypeBias);
            const auto index = static_cast<std::uint16_t>(indexRaw - kIndexBias);
            if (!update.hasFirstObject) {
                update.firstGroupBits = static_cast<std::uint32_t>(groupBits);
                update.firstRegistryKey = static_cast<std::uint32_t>(groupKey);
                update.firstSlotType = type;
                update.firstSlotIndex = index;
                update.hasFirstObject = true;
            }
            DecodedObject* retained = nullptr;
            if (update.decoded.objectCount < update.decoded.objects.size()) {
                retained = &update.decoded.objects[update.decoded.objectCount++];
                retained->registryKey = static_cast<std::uint32_t>(groupKey);
                retained->objectTag = group.objectTag;
                retained->objectRow = group.objectRow;
                retained->slotType = type;
                retained->slotIndex = index;
                retained->firstValue = static_cast<std::uint32_t>(update.decoded.valueCount);
            } else {
                update.decoded.objectsTruncated = true;
            }
            SlotTarget slot{};
            const TargetStatus slotStatus =
                resolver.resolveSlot == nullptr
                    ? TargetStatus::targetUnavailable
                    : resolver.resolveSlot(resolver.context, group, type, index, slot);
            if (retained != nullptr) {
                retained->slotRow = slot.slotRow;
                retained->senseSchema = slot.senseSchema;
                retained->schemaRow = slot.senseSchema;
            }
            if (slotStatus != TargetStatus::resolved) {
                if (retained != nullptr) {
                    retained->status = slotStatus == TargetStatus::schemaUnavailable
                                           ? ObjectStatus::schemaUnavailable
                                           : ObjectStatus::targetUnavailable;
                }
                if (!skip_group(body)) {
                    finish(update, reader, total, DecodeStatus::malformed, consumed);
                    return false;
                }
                skipped = true;
                partial = true;
                ++update.decoded.groupsSkipped;
                continue;
            }
            const std::size_t start = body.position();
            Values values(update.decoded, retained);
            const NativeStatus status = decode_body(slot.senseSchema, body, values);
            if (retained != nullptr) {
                retained->deltaBits = static_cast<std::uint32_t>(body.position() - start);
                retained->valueCount =
                    static_cast<std::uint32_t>(update.decoded.valueCount - retained->firstValue);
                retained->status = status == NativeStatus::complete      ? ObjectStatus::decoded
                                   : status == NativeStatus::unsafeCount ? ObjectStatus::unsafeCount
                                   : status == NativeStatus::unsupported
                                       ? ObjectStatus::unsupportedField
                                       : ObjectStatus::malformed;
            }
            if (status == NativeStatus::malformed) {
                finish(update, reader, total, DecodeStatus::malformed, consumed);
                return false;
            }
            if (status != NativeStatus::complete) {
                if (!skip_group(body)) {
                    finish(update, reader, total, DecodeStatus::malformed, consumed);
                    return false;
                }
                skipped = true;
                partial = true;
                ++update.decoded.groupsSkipped;
                continue;
            }
            std::uint64_t generation = 0;
            if (!body.read(32, generation)) {
                finish(update, reader, total, DecodeStatus::malformed, consumed);
                return false;
            }
            if (retained != nullptr) {
                retained->generationPlusOne = static_cast<std::uint32_t>(generation);
                retained->hasGeneration = true;
                retained->status = ObjectStatus::decoded;
            }
            ++update.decoded.objectsDecoded;
        }
    }
    std::uint64_t tail = 0;
    if (!reader.read(1, tail) || tail != 0 || reader.remaining_bits() > 7) {
        finish(update, reader, total, DecodeStatus::malformed, consumed);
        return false;
    }
    update.decoded.paddingBits = static_cast<std::uint8_t>(reader.remaining_bits());
    std::uint64_t padding = 0;
    if (!reader.read(update.decoded.paddingBits, padding) || padding != 0) {
        finish(update, reader, total, DecodeStatus::malformed, consumed);
        return false;
    }
    finish(
        update, reader, total, partial ? DecodeStatus::partial : DecodeStatus::complete, consumed);
    return true;
}

/** @return Stable log spelling for one packet decode status. */
const char* decode_status_name(DecodeStatus status) noexcept {
    switch (status) {
    case DecodeStatus::complete:
        return "complete";
    case DecodeStatus::partial:
        return "partial";
    case DecodeStatus::schemaUnavailable:
        return "unsupported_native_layout";
    case DecodeStatus::malformed:
        return "malformed";
    }
    return "unknown";
}
/** @return Stable log spelling for one object decode status. */
const char* object_status_name(ObjectStatus status) noexcept {
    switch (status) {
    case ObjectStatus::decoded:
        return "decoded";
    case ObjectStatus::targetUnavailable:
        return "target_unavailable";
    case ObjectStatus::schemaUnavailable:
    case ObjectStatus::unsupportedField:
        return "unsupported_native_layout";
    case ObjectStatus::unsafeCount:
        return "unsafe_count";
    case ObjectStatus::malformed:
        return "malformed";
    }
    return "unknown";
}
} // namespace sunrise::middleware::bap::activity_message::sense_update
