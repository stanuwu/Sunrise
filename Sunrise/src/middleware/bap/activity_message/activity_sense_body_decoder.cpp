#include <bit>
#include <limits>

#include "../../content/packages/schema/generated/tag_classes_80804d.h"
#include "../../content/packages/schema/generated/tag_classes_80804f.h"
#include "../../content/packages/schema/generated/tag_classes_808062.h"
#include "../../content/packages/schema/generated/tag_classes_80807d.h"
#include "../../content/packages/schema/generated/tag_classes_80807e.h"
#include "../../content/packages/schema/generated/tag_classes_80807f.h"
#include "../../content/packages/schema/generated/tag_classes_808094.h"
#include "../../content/packages/schema/generated/tag_classes_808095.h"
#include "../../content/packages/schema/generated/tag_classes_808099.h"
#include "../../content/packages/schema/generated/tag_classes_80809a.h"
#include "../../content/packages/schema/generated/tag_classes_80809e.h"
#include "damage_monitor_auth.h"
#include "ghost_link_auth.h"
#include "interactable_object_auth.h"
#include "scriptable_auth_body.h"
#include "sense_update_body.h"
#include "squad_sense_state.h"

// The per-schema msg-6 Sense body readers. Each reads one root body inside its object budget.

namespace sunrise::middleware::bap::activity_message::sense_update::body {
namespace {

namespace generated = middleware::content::packages::schema::generated;

/** Schema tags the sense-update objects and their nested arrays declare. */
constexpr std::uint32_t kDevice = generated::C80804f47::kClass,
                        kScene = generated::C8080626a::kClass;
constexpr std::uint32_t kSquad = squad_sense::kSchema, kObjective = generated::C80807f04::kClass;
constexpr std::uint32_t kOccupancy = generated::C80809531::kClass;
constexpr std::uint32_t kAttachment = generated::C8080954a::kClass;
constexpr std::uint32_t kGhostLink = ghost_link::kSenseSchema;
constexpr std::uint32_t kObject = generated::C8080992e::kClass;
constexpr std::uint32_t kObjectSpawnMask = generated::C80809e1b::kClass,
                        kObjectReplies = generated::C80809aea::kClass;
constexpr std::uint32_t kSceneList = generated::C808094e1::kClass;
/** A u32 key list; scene events and player keys use the same schema. */
constexpr std::uint32_t kKeyList = generated::C808094df::kClass;
constexpr std::uint32_t kSquadCounts = generated::C80809491::kClass,
                        kSquadList = generated::C80807ecf::kClass;
constexpr std::uint32_t kSquadReals = generated::C80807ecd::kClass;
constexpr std::uint32_t kObjectiveBlock = generated::C80807f07::kClass,
                        kObjectiveTasks = generated::C80807f08::kClass;
constexpr std::uint32_t kCombatant = scriptable_auth::kType2SenseSchema,
                        kCombatantAtoms = generated::C80807f6e::kClass;
constexpr std::uint32_t kCombatantKeyed = generated::C80807da3::kClass,
                        kCombatantLanes = generated::C80807da4::kClass;
constexpr std::uint32_t kEngagement = generated::C808094f0::kClass,
                        kEngagementList = generated::C808094f8::kClass;
/** The engagement list: a five-bit element count that the native list caps at 16. */
constexpr std::uint8_t kEngagementCountWidth = 5;
constexpr std::uint64_t kEngagementCapacity = 16;
/** Engagement ordinal 1 is a 16-bit value biased by 32768. */
constexpr std::uint16_t kEngagementValueOrdinal = 1;
constexpr std::uint8_t kEngagementValueWidth = 16;
constexpr std::int32_t kEngagementValueBias = 32'768;
/** The type-26 attachment body: three required signed 32-bit counters, then one flag. */
constexpr std::uint16_t kAttachmentCounterCount = 3;
constexpr std::uint8_t kAttachmentCounterWidth = 32;
constexpr std::uint16_t kAttachmentFlagOrdinal = kAttachmentCounterCount;
/** Type-13 participation root and its nested blocks, in body order. */
constexpr std::uint32_t kPlayer = generated::C80804f2f::kClass,
                        kPlayerState = generated::C808094e4::kClass;
constexpr std::uint32_t kPlayerScalars = generated::C80804f39::kClass,
                        kPlayerTail = generated::C80804f35::kClass;
constexpr std::uint32_t kPlayerIdentity = generated::C808094dd::kClass;
constexpr std::uint32_t kPlayerPairs = generated::C808094e3::kClass,
                        kPlayerFlags = generated::C80804f31::kClass;
constexpr std::uint32_t kPlayerOptional = generated::C80804f34::kClass;
/** Type-20 damage monitor root: health, shield, echoed revision. */
constexpr std::uint32_t kDamage = damage_monitor::kSenseSchema;
/** Registered object replies: the interaction latch and the ownership key. */
constexpr std::uint32_t kInteractionReply = interactable_object::kInteractionReply,
                        kOwnershipReply = interactable_object::kOwnershipReply;
/** Counted lists inside the participation body. */
constexpr std::uint64_t kPlayerScalarCountMaximum = 4, kPlayerKeyCountMaximum = 32;
/** Quantization ceilings the combatant reals carry in their own descriptors. */
constexpr std::uint32_t kSpatialMaximumBits = 0x45000000U, kUnitMaximumBits = 0x3F800000U;

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

/** Required fields are present; optional fields consume one presence bit within the budget. */
[[nodiscard]] bool present(Reader& reader, bool optional, bool& output) noexcept {
    output = true;
    if (!optional) {
        return true;
    }
    std::uint64_t raw = 0;
    if (!reader.read(1, raw)) {
        return false;
    }
    output = raw != 0;
    return true;
}
/** Records an unsigned value only when its presence bit and selected value fit. */
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
        if (!read_unsigned(reader, values, kKeyList, 0, 32, false, index)) {
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
 * Decodes the engagement body. Only an empty element list is supported: the element codec is
 * unknown, and its storage size says nothing about its wire width.
 */
[[nodiscard]] NativeStatus decode_engagement(Reader& reader, Values& values) noexcept {
    std::uint64_t count = 0;
    const auto at = static_cast<std::uint32_t>(reader.position());
    if (!reader.read(kEngagementCountWidth, count)) {
        return NativeStatus::malformed;
    }
    if (count > kEngagementCapacity) {
        return NativeStatus::unsafeCount;
    }
    if (count != 0) {
        return NativeStatus::unsupported;
    }
    values.put(kEngagementList,
               0,
               0,
               at,
               kEngagementCountWidth,
               ValueKind::unsignedInteger,
               count,
               0,
               0.0F,
               true);
    return read_signed(reader,
                       values,
                       kEngagement,
                       kEngagementValueOrdinal,
                       kEngagementValueWidth,
                       kEngagementValueWidth,
                       kEngagementValueBias,
                       false)
               ? NativeStatus::complete
               : NativeStatus::malformed;
}

/** Decodes the type-26 attachment body. */
[[nodiscard]] NativeStatus decode_attachment(Reader& reader, Values& values) noexcept {
    for (std::uint16_t ordinal = 0; ordinal < kAttachmentCounterCount; ++ordinal) {
        if (!read_signed(reader,
                         values,
                         kAttachment,
                         ordinal,
                         kAttachmentCounterWidth,
                         kAttachmentCounterWidth,
                         (std::numeric_limits<std::int32_t>::min)(),
                         false)) {
            return NativeStatus::malformed;
        }
    }
    return read_bool(reader, values, kAttachment, kAttachmentFlagOrdinal, false)
               ? NativeStatus::complete
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
        if (!read_unsigned(reader, values, kKeyList, 0, 32, false, index)) {
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

} // namespace

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

} // namespace sunrise::middleware::bap::activity_message::sense_update::body
