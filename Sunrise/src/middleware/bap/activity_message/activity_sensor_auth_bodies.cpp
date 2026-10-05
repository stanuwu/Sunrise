#include "auth_schema_catalog.h"
#include "map_generator_auth.h"
#include "sensor_auth_update.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {
namespace {

namespace bits = encoding::bits;

/** Slot types whose auth body this module fills. Every other block is seed-only. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
constexpr std::uint8_t kSlotTypeLifetime = 17;
constexpr std::uint8_t kSlotTypeConfiguration = 8;
constexpr std::uint8_t kSlotTypePackage = 16;
constexpr std::uint8_t kSlotTypeQueues = 41;
constexpr std::uint8_t kSlotTypeSpawnKeys = 67;
/** Map generator, announced on every region. */
constexpr std::uint8_t kSlotTypeMapGenerator = 37;
/** Player monitor, which a published room object brings into the stream. */
constexpr std::uint8_t kSlotTypePlayerMonitor = 30;
/** Hard-wipe globals. Its body is what an encounter bubble's script objects come from. */
constexpr std::uint8_t kSlotTypeHardWipeGlobals = 35;
/** Activity timer, which ships beside the hard-wipe globals in the same group. */
constexpr std::uint8_t kSlotTypeActivityTimer = 18;

/** Body widths, each checked against the writer after the body is written. */
constexpr std::size_t kParticipationBits = 192;
constexpr std::size_t kParticipationRegionBits = 32;
constexpr std::size_t kLifetimeBits = 520;
constexpr std::size_t kConfigurationBits = 35;
constexpr std::size_t kPackageBits = 7;
constexpr std::size_t kQueueBits = 12;
constexpr std::size_t kSpawnKeyBits = 32 * 32 + 1 + 32;
/** Optional participation revive delay, a 16-bit half float: 30 s under darkness, else 3 s. */
constexpr std::uint8_t kHalfWidth = 16;
constexpr std::uint32_t kDarknessReviveDelayHalf = 0x4F80U;
constexpr std::uint32_t kDefaultReviveDelayHalf = 0x4200U;
/** A packed region is bubble times eight plus the state ordinal. */
constexpr std::uint32_t kStatesPerBubble = 8;
/**
 * Lifetime field `.2`: whether the activity admits fireteam joins and invites. The client stores
 * the bit at roster object `+0x180+2`; a zero locks its fireteam session (closed flag 8), so a
 * member who returns to orbit is refused `not-joinable` on the way back in.
 */
constexpr std::uint32_t kActivityAllowsFireteamJoin = 1;
/**
 * Empty map-generator body, schema `0x80805007`.
 * Two 475-bit records, a u32, then fixed arrays of 32 and 64 u8. The fixed array lengths apply
 * even with both dynamic arrays empty; counting one byte each truncates the body by 752 bits.
 */
constexpr std::size_t kMapGeneratorBits = map_generator_auth::kBitCount;
/**
 * Player-monitor body, schema `0x80809532`: a slot reference then a biased i32.
 * Fixed width: no presence bit and no array, so there is exactly one legal length.
 */
constexpr std::size_t kPlayerMonitorBits = 32 + 7 + 16 + 32;
/** Clock window, schema `0x808099C4`: a bool, five u64 and a raw u32, every field unbiased. */
constexpr std::size_t kClockWindowBits = 1 + 5 * 64 + 32;
/** Words in the clock window. */
constexpr std::size_t kClockWindowWords = 5;
/** Hard-wipe globals body: two bools, two bias-1 selectors, then the clock window. */
constexpr std::size_t kHardWipeGlobalsBits = 1 + 1 + 2 + 2 + kClockWindowBits;
/** Activity-timer body: the clock window, a bool, then one biased signed word. */
constexpr std::size_t kActivityTimerBits = kClockWindowBits + 1 + 32;
/** Width of the hard-wipe selectors, each a signed byte biased by one. */
constexpr std::uint8_t kHardWipeSelectorWidth = 2;
/** Wire value for a zero selector. A wire zero decodes to -1, the none sentinel. */
constexpr std::uint32_t kHardWipeSelectorZero = 1;
/** Type 30's 7-bit field carries a bias of one, so this wire value decodes to zero. */
constexpr std::uint32_t kPlayerMonitorSelectorZero = 1;
/** Type 30's 16-bit field carries a bias of 0x8000, so this wire value decodes to zero. */
constexpr std::uint32_t kUnsignedShortZero = 0x8000;

/** @return Least legal wire width of one slot type, or zero when the census has no row. */
[[nodiscard]] constexpr std::size_t census_minimum_bits(std::uint8_t slotType) noexcept {
    for (const auth_schema_catalog::Type& type : auth_schema_catalog::kTypes) {
        if (type.slotType == slotType) {
            return type.minimumBits;
        }
    }
    return 0;
}

// A composed width that drifts from the census truncates the block and desynchronises the stream.
static_assert(kMapGeneratorBits == census_minimum_bits(kSlotTypeMapGenerator));
static_assert(kPlayerMonitorBits == census_minimum_bits(kSlotTypePlayerMonitor));
static_assert(kHardWipeGlobalsBits == census_minimum_bits(kSlotTypeHardWipeGlobals));
static_assert(kActivityTimerBits == census_minimum_bits(kSlotTypeActivityTimer));

/** Signed fields in these bodies carry a -2^31 bias, so this wire value stores zero. */
constexpr std::uint32_t kSignedZero = 0x80000000;
/** The same bias wraps at the top of the field, so this wire value stores -1. */
constexpr std::uint32_t kSignedMinusOne = 0x7FFFFFFF;
/** The region index rides the same bias, so its wire value is the bias plus the index. */
constexpr std::uint32_t kRegionBias = 0x80000000;
/** Message 52's team-state byte 1, where bit 1 is `awaiting_client_sync`. */
constexpr std::uint32_t kAwaitingClientSync = 2;
/** Neutral runtime-i32 override that forces the type-17 waiting selector to zero. */
constexpr std::uint32_t kWaitingSwitchKey = 0xB3C1251B;
constexpr std::uint32_t kWaitingSwitchClass = 0x80800007;
/** Type 17 carries 3 spawn overrides. Wire zero stores index -1 and disables one. */
constexpr std::size_t kSpawnOverrideCount = 3;
constexpr std::uint8_t kSpawnOverrideIndexWidth = 10;
constexpr std::uint32_t kSpawnOverrideIndexBias = 1;
/** Type 67 maps the 32 spawn-key ordinals to themselves, matching its constructor. */
constexpr std::size_t kSpawnKeyCount = 32;

/**
 * Writes the participation body, which binds the player and latches the region.
 * Every biased field must carry its bias; a zero-filled field decodes to the smallest signed value.
 * @param writer Body writer.
 * @param snapshot Message input.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_participation(bits::Writer& writer,
                                       const Snapshot& snapshot,
                                       std::uint64_t playerKey) noexcept {
    // An optional field's value follows its presence bit, so sending +0 shifts everything below.
    bool encoded = writer.write(snapshot.hasRegion ? 1U : 0U, kPresenceWidth);
    if (encoded && snapshot.hasRegion) {
        encoded = writer.write(kRegionBias + snapshot.region, kParticipationRegionBits);
    }
    // The participation record is this body's head, so struct +8 and +10 are record +8 and +10.
    // Record +8 is step 36 task 9's own term and +10 is the spawn gate's.
    // Record +56's assignment, 5 bits at bias 1, must equal the membership blob's A.P2 value.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(1, kPresenceWidth)
           && writer.write(1, kPresenceWidth) && writer.write(1, kPresenceWidth)
           && writer.write(0, kPresenceWidth) && writer.write(1, 3) && writer.write(1, 2)
           && writer.write(0, 3) && writer.write(0, 32) && writer.write(1, 5)
           && writer.write(0, kPresenceWidth) && writer.write(0, 3)
           && writer.write(1, kPresenceWidth)
           && writer.write(playerKey != 0 ? playerKey : snapshot.playerKey, 64)
           && writer.write(0, 5) && writer.write(3, 6) && writer.write(0, 6)
           && writer.write(0, 6)
           // Byte 736 skips the late spawn-location hold. Byte 737 holds the spawn while the
           // client loads. The revive delay stays authored unless a darkness policy is set.
           && writer.write(1, kPresenceWidth)
           && writer.write(snapshot.awaitClientSync ? kAwaitingClientSync : 0U, 4)
           && writer.write(0, kPresenceWidth)
           && writer.write(snapshot.hasDarknessPolicy ? 1U : 0U, kPresenceWidth)
           && (!snapshot.hasDarknessPolicy
               || writer.write(snapshot.darknessEnabled ? kDarknessReviveDelayHalf
                                                        : kDefaultReviveDelayHalf,
                               kHalfWidth))
           && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
           && writer.write(128, 8) && writer.write(kSignedZero, 32);
}

/**
 * Writes the lifetime body, which is the activity state the roster reports.
 * @param writer Body writer.
 * @param snapshot Message input.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_lifetime(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    // Field `.4` names an authored spawn entry. No host model owns one, so it carries the empty
    // name hash; zero is a hash that no row matches.
    bool encoded =
        writer.write(std::uint32_t{snapshot.lifetime} + kLifetimeBias, kLifetimeWidth)
        && writer.write(1, 3) && writer.write(kActivityAllowsFireteamJoin, kPresenceWidth)
        && writer.write(kSignedZero, 32)
        && writer.write(kEmptyNameHash, 32)
        // `.5` names the published region's bubble; a disabled darkness policy sends -1.
        && writer.write(snapshot.hasDarknessPolicy && !snapshot.darknessEnabled ? kSignedMinusOne
                        : snapshot.hasRegion ? kSignedZero + snapshot.region / kStatesPerBubble
                        : snapshot.hasDarknessPolicy ? kSignedMinusOne
                                                     : kSignedZero,
                        32)
        && writer.write(1, 6) && writer.write(kWaitingSwitchKey, 32)
        && writer.write(1, kPresenceWidth) && writer.write(kWaitingSwitchClass, 32)
        && writer.write(kSignedZero, 32) && writer.write(kSignedZero, 32);
    for (std::size_t index = 0; encoded && index < kSpawnOverrideCount; ++index) {
        const std::uint32_t slice =
            snapshot.hasSpawnOverride ? snapshot.spawnSliceSet + kSpawnOverrideIndexBias : 0U;
        const std::uint32_t hash =
            snapshot.hasSpawnOverride ? snapshot.spawnSetHash : kAbsentSpawnSetHash;
        encoded = writer.write(slice, kSpawnOverrideIndexWidth) && writer.write(hash, 32);
    }
    // Struct `+1256` is the out-of-bounds `activity_quarantine` selector. The reader arms the
    // quarantine at or below 0x3F unsigned, so minus one leaves it clear and teleports nobody.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(0, 32)
           && writer.write(kSignedMinusOne, 32) && writer.write(0, 32)
           && writer.write(kSlotTypeBias, kSlotTypeWidth)
           && writer.write(kSlotIndexBias, kSlotIndexWidth) && writer.write(0, 32)
           && writer.write(0, 3);
}

/**
 * Writes the clock window shared by the hard-wipe globals and activity-timer bodies.
 * Every field is unbiased, so zeros store literal zeros and leave the timer stopped.
 * @param writer Body writer.
 * @return True when the record fits.
 */
[[nodiscard]] bool write_clock_window(bits::Writer& writer) noexcept {
    bool encoded = writer.write(0, kPresenceWidth);
    for (std::size_t word = 0; encoded && word < kClockWindowWords; ++word) {
        encoded = writer.write(0, 64);
    }
    return encoded && writer.write(0, 32);
}

/**
 * Writes the hard-wipe globals body, schema `0x808099BF`.
 * Fixed width: no presence bit, no array and no variant field, so one legal length.
 * @param writer Body writer.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_hard_wipe_globals(bits::Writer& writer) noexcept {
    // TODO: the client seeds the second selector to -1; confirm zero is the value to assert.
    return writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
           && writer.write(kHardWipeSelectorZero, kHardWipeSelectorWidth)
           && writer.write(kHardWipeSelectorZero, kHardWipeSelectorWidth)
           && write_clock_window(writer);
}

/**
 * Writes the activity-timer body, schema `0x80809919`.
 * The trailing signed word carries the same `+2^31` bias as the other signed fields here.
 * @param writer Body writer.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_activity_timer(bits::Writer& writer) noexcept {
    return write_clock_window(writer) && writer.write(0, kPresenceWidth)
           && writer.write(kSignedZero, 32);
}

/**
 * Writes the spawn-key body, which maps the 32 ordinals to themselves.
 * @param writer Body writer.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_spawn_keys(bits::Writer& writer) noexcept {
    bool encoded = true;
    for (std::size_t index = 0; encoded && index < kSpawnKeyCount; ++index) {
        encoded = writer.write(kSignedZero + index, 32);
    }
    return encoded && writer.write(0, kPresenceWidth) && writer.write(kSignedMinusOne, 32);
}

} // namespace

/** Reports how many bits of auth body one slot carries. */
std::size_t
auth_body_bits(const Snapshot& snapshot, std::uint8_t slotType, bool carriesPlayerKey) noexcept {
    if (slotType == kSlotTypeParticipation) {
        return carriesPlayerKey
                   ? kParticipationBits + (snapshot.hasRegion ? kParticipationRegionBits : 0)
                         + (snapshot.hasDarknessPolicy ? kHalfWidth : 0U)
                   : 0;
    }
    if (slotType == scoreboard_record::kSlotType && snapshot.hasScoreboard) {
        return scoreboard_record::record_body_bits(snapshot.scoreboard);
    }
    if (slotType == kSlotTypeLifetime) {
        return kLifetimeBits;
    }
    if (slotType == kSlotTypeConfiguration) {
        return kConfigurationBits;
    }
    if (slotType == kSlotTypePackage) {
        return kPackageBits;
    }
    if (slotType == kSlotTypeQueues) {
        return kQueueBits;
    }
    if (slotType == kSlotTypeSpawnKeys) {
        return kSpawnKeyBits;
    }
    if (slotType == kSlotTypeHardWipeGlobals) {
        return kHardWipeGlobalsBits;
    }
    if (slotType == kSlotTypeActivityTimer) {
        return kActivityTimerBits;
    }
    if (slotType == kSlotTypeMapGenerator) {
        return kMapGeneratorBits;
    }
    if (slotType == kSlotTypePlayerMonitor) {
        return kPlayerMonitorBits;
    }
    return 0;
}

/** Writes one slot's auth body. */
bool write_auth_body(bits::Writer& writer,
                     const Snapshot& snapshot,
                     std::uint8_t slotType,
                     bool carriesPlayerKey,
                     std::uint64_t playerKey) noexcept {
    const std::size_t start = writer.bit_count();
    const std::size_t expected = auth_body_bits(snapshot, slotType, carriesPlayerKey);
    bool encoded = true;
    if (slotType == kSlotTypeParticipation && carriesPlayerKey) {
        encoded = write_participation(writer, snapshot, playerKey);
    } else if (slotType == scoreboard_record::kSlotType && snapshot.hasScoreboard) {
        encoded = scoreboard_record::write_record_body(writer, snapshot.scoreboard);
    } else if (slotType == kSlotTypeLifetime) {
        encoded = write_lifetime(writer, snapshot);
    } else if (slotType == kSlotTypeConfiguration) {
        // Both optional arrays absent and the terminal tag clear is the constructed state.
        encoded = writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                  && writer.write(0, kPresenceWidth) && writer.write(0, 32);
    } else if (slotType == kSlotTypePackage) {
        // 7 absent top-level fields keep the package-owned configuration.
        encoded = pad_bits(writer, kPackageBits);
    } else if (slotType == kSlotTypeQueues) {
        encoded = writer.write(0, 7) && writer.write(0, 5);
    } else if (slotType == kSlotTypeSpawnKeys) {
        encoded = write_spawn_keys(writer);
    } else if (slotType == kSlotTypeHardWipeGlobals) {
        encoded = write_hard_wipe_globals(writer);
    } else if (slotType == kSlotTypeActivityTimer) {
        encoded = write_activity_timer(writer);
    } else if (slotType == kSlotTypePlayerMonitor) {
        // Three of the four fields carry a bias, so a neutral body writes each bias, not a zero.
        encoded = writer.write(0, 32) && writer.write(kPlayerMonitorSelectorZero, 7)
                  && writer.write(kUnsignedShortZero, 16) && writer.write(kSignedZero, 32);
    } else if (slotType == kSlotTypeMapGenerator) {
        encoded = map_generator_auth::write_body(writer, {});
    }
    return encoded && writer.bit_count() == start + expected;
}

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
