#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_80804f.h"
#include "../../content/packages/schema/generated/tag_classes_808099.h"
#include "../../content/packages/schema/generated/tag_classes_80809a.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"

// Type-4 object Auth that spawns authored entry zero and subscribes to accepted player use.
// The client answers the subscription on its Sense reply list with the interaction latch.

namespace sunrise::middleware::bap::activity_message::interactable_object {

namespace fields = auth_fields;
namespace generated = middleware::content::packages::schema::generated;

/** Object Auth schema: the type-4 slot's Auth schema. */
inline constexpr std::uint32_t kSchema = scriptable_auth::kType4Schema;
/** Registered reply the client emits for the interaction subscription below. */
inline constexpr std::uint32_t kInteractionReply = generated::C80804fb7::kClass;
/** Subscription schema that makes the client emit kInteractionReply. */
inline constexpr std::uint32_t kInteractionSubscription = generated::C80804fb8::kClass;
/** Optional ownership subscription; the client answers it with the owner key reply. */
inline constexpr std::uint32_t kOwnershipSubscription = generated::C80809acd::kClass;
inline constexpr std::uint32_t kOwnershipReply = generated::C80809acc::kClass;
inline constexpr std::size_t kBits = 375;
/** The ownership subscription adds its presence bit and schema. */
inline constexpr std::size_t kOwnerBits = kBits + fields::kPresenceWidth + 32;
/** Record count: the interaction subscription, then ownership and the timer when sent. */
inline constexpr std::uint8_t kSubscriptionCountWidth = 2;
/**
 * Timer record. The object's timer component copies it whole, shows elapsed / (elapsed +
 * remaining), and fires its completed request once that reaches 1 while active. Nothing else
 * writes it, so only the server starts it.
 */
inline constexpr std::uint32_t kTimerRecord = generated::C80804fca::kClass;
/** Clock fields are 64-bit activity ticks; the rate is a 32-bit float. */
inline constexpr std::uint8_t kTickWidth = 64;
inline constexpr std::uint8_t kRateWidth = 32;
/** The timed state's clock fields: minimum, maximum, elapsed, remaining and anchor. */
inline constexpr std::size_t kTimerClockFieldCount = 5;
/** Active, then the timed state (running, its clock fields, rate), then one trailing flag. */
inline constexpr std::size_t kTimerPayloadBits = fields::kBoolWidth + fields::kBoolWidth
                                                 + kTimerClockFieldCount * kTickWidth + kRateWidth
                                                 + fields::kBoolWidth;
inline constexpr std::size_t kTimerRecordBits = fields::kPresenceWidth + 32 + kTimerPayloadBits;
inline constexpr std::size_t kMaximumBits = kOwnerBits + kTimerRecordBits;
inline constexpr std::size_t kMaximumBytes = (kMaximumBits + 7) / 8;
/** A stopped clock is not anchored to the client's activity clock. */
inline constexpr std::uint64_t kNoAnchor = (std::numeric_limits<std::uint64_t>::max)();
/** The timer's clock runs at the activity clock's own speed. */
inline constexpr float kTimerRate = 1.0F;

/** A stopped timer: the component reads elapsed / (elapsed + remaining) as it stands. */
struct Timer final {
    bool active{true};
    std::uint64_t elapsedTicks{};
    std::uint64_t remainingTicks{};
};

/** The records and states one object body carries beside its generation. */
struct Options final {
    /** Adds the ownership subscription. */
    bool trackOwner{};
    /** Field .2, whether the entry is active. */
    bool active{true};
    /** Sends the row already latched at revision one, the state after a first use. */
    bool used{};
    /** False keeps the entry where it is but lets no player use it. */
    bool usable{true};
    /** Adds the timer record; minimum 0 and maximum elapsed + remaining. */
    std::optional<Timer> timer{};
};

/** @return The body's meaningful width with the optional records it carries. */
[[nodiscard]] constexpr std::size_t bit_count(const Options& options) noexcept {
    return (options.trackOwner ? kOwnerBits : kBits)
           + (options.timer.has_value() ? kTimerRecordBits : 0U);
}
/**
 * Use-visibility mode. Wire zero is native -1, which lets use through with no player filter. Wire
 * two is native 1: with no player filter the client lets no player use the entry, which stays in
 * place.
 */
inline constexpr std::uint8_t kUseVisibilityWidth = 2;
inline constexpr std::uint32_t kUseVisibilityDefault = 0;
inline constexpr std::uint32_t kUseVisibilityNobody = 2;

/**
 * Encodes the object body.
 * @param generation Positive object generation; a new one respawns the entry.
 * @param written Receives the byte count of bit_count(options).
 */
[[nodiscard]] inline bool encode(std::int32_t generation,
                                 std::span<std::byte> output,
                                 std::size_t& written,
                                 const Options& options = {}) noexcept {
    written = 0;
    const std::size_t bits = bit_count(options);
    const std::size_t bytes = (bits + 7) / 8;
    const std::optional<Timer>& timer = options.timer;
    if (generation <= 0 || output.size() < bytes
        || (timer.has_value()
            && timer->remainingTicks
                   > (std::numeric_limits<std::uint64_t>::max)() - timer->elapsedTicks)) {
        return false;
    }
    const std::uint32_t records =
        1U + (options.trackOwner ? 1U : 0U) + (timer.has_value() ? 1U : 0U);
    encoding::bits::Writer writer(output.first(bytes));
    const std::array<fields::Field, 5> head{{
        {static_cast<std::uint32_t>(generation) + fields::kSigned32Bias, 32},
        {fields::kSigned32Bias, 32}, // signed zero
        {options.active ? 1U : 0U, fields::kBoolWidth},
        {0, fields::kBoolWidth},
        {fields::kSigned32Bias, 32}, // signed zero
    }};
    const std::array<fields::Field, 7> transform{{
        {0, 32},
        {0, 32},
        {0, 32},
        {0, fields::kBoolWidth},
        {records, kSubscriptionCountWidth},
        {1, fields::kPresenceWidth},
        {kInteractionSubscription, 32},
    }};
    const std::array<fields::Field, 1> visibility{
        {{options.usable ? kUseVisibilityDefault : kUseVisibilityNobody, kUseVisibilityWidth}}};
    // The row's revision and latch; the client takes them only when the revision is newer.
    const std::array<fields::Field, 2> tail{{
        {fields::kSigned32Bias + (options.used ? 1U : 0U), 32},
        {options.used ? 1U : 0U, fields::kBoolWidth},
    }};
    return fields::write_fields(writer, head) && fields::write_absent_client_ref(writer)
           && fields::write_fields(writer, transform) && fields::write_fields(writer, visibility)
           && fields::write_absent_client_ref(writer) && fields::write_fields(writer, tail)
           && (!options.trackOwner
               || (writer.write(1, fields::kPresenceWidth)
                   && writer.write(kOwnershipSubscription, 32)))
           && (!timer.has_value()
               || (writer.write(1, fields::kPresenceWidth) && writer.write(kTimerRecord, 32)
                   && writer.write(timer->active ? 1U : 0U, fields::kBoolWidth)
                   && writer.write(0, fields::kBoolWidth) // stopped
                   && writer.write(0, kTickWidth)         // minimum
                   && writer.write(timer->elapsedTicks + timer->remainingTicks, kTickWidth)
                   && writer.write(timer->elapsedTicks, kTickWidth)
                   && writer.write(timer->remainingTicks, kTickWidth)
                   && writer.write(kNoAnchor, kTickWidth)
                   && writer.write(std::bit_cast<std::uint32_t>(kTimerRate), kRateWidth)
                   && writer.write(0, fields::kBoolWidth)))
           && fields::finish_exact(writer, bits, bytes, written);
}

} // namespace sunrise::middleware::bap::activity_message::interactable_object
