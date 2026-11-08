#include "squad_sense_state.h"

#include <limits>

#include "../../content/packages/schema/generated/tag_classes_80807e.h"
#include "../../content/packages/schema/generated/tag_classes_808094.h"
#include "../../encoding/bit_writer.h"

namespace sunrise::middleware::bap::activity_message::squad_sense {
namespace {

namespace generated = middleware::content::packages::schema::generated;

using sense_update::DecodedValue;
using sense_update::ValueKind;
/** Nested native schemas for the list, its signed counts, and quantized real array. */
constexpr std::uint32_t kListSchema = generated::C80807ecf::kClass,
                        kCountSchema = generated::C80809491::kClass;
constexpr std::uint32_t kRealSchema = generated::C80807ecd::kClass;
/** Root fields 0 through 5 carry these wire widths; field 4 is a quantized real. */
constexpr std::array<std::uint8_t, kScalarCount> kScalarWidths{31, 31, 31, 6, 7, 31};
constexpr std::size_t kRealField = 4;
/** The two signed bytes use bias one; nested counts use the signed 32-bit minimum. */
constexpr std::uint32_t kByteBias = 1, kCountBias = 0x80000000U;
/** Root and both nested presence bits are always on the wire. */
constexpr std::size_t kPresenceBits = 3;
/** Nested count and real widths are fixed by their native schemas. */
constexpr std::uint8_t kCountWidth = 4, kSignedWidth = 32, kRealWidth = 7;
/** Required root fields occupy ordinals 6 through 10. */
constexpr std::uint16_t kField6 = 6, kField7 = 7, kField8 = 8, kField9 = 9;
constexpr std::uint16_t kInitializedField = 10;

/** Validates a scalar's native identity before any persistent state changes. */
bool matches(const DecodedValue& value,
             std::uint32_t schema,
             std::uint16_t ordinal,
             std::uint32_t occurrence,
             ValueKind kind,
             std::uint8_t width,
             bool optional) noexcept {
    return value.schemaRow == schema && value.fieldOrdinal == ordinal
           && value.occurrence == occurrence && value.kind == kind
           && (value.present ? value.width == width : optional && value.width == 0);
}

/** Reads the decoder's exact field order and accounts for every retained wire bit. */
class Cursor final {
public:
    explicit Cursor(std::span<const DecodedValue> values) noexcept : values_(values) {}

    /**
     * Refuses missing, reordered, or out-of-range scalar rows.
     * @param schema Native schema hash.
     * @param ordinal Native field ordinal.
     * @param occurrence Array index.
     * @param kind Scalar domain.
     * @param width Native wire width.
     * @param optional Whether absence is legal.
     * @return The validated row or null.
     */
    const DecodedValue* take(std::uint32_t schema,
                             std::uint16_t ordinal,
                             std::uint32_t occurrence,
                             ValueKind kind,
                             std::uint8_t width,
                             bool optional = false) noexcept {
        if (values_.empty()
            || !matches(values_.front(), schema, ordinal, occurrence, kind, width, optional)) {
            return nullptr;
        }
        const auto* value = &values_.front();
        values_ = values_.subspan(1);
        bits += (optional ? 1U : 0U) + value->width;
        return value;
    }

    bool next_schema(std::uint32_t schema) const noexcept {
        return !values_.empty() && values_.front().schemaRow == schema;
    }
    bool empty() const noexcept {
        return values_.empty();
    }
    std::size_t bits{kPresenceBits};

private:
    std::span<const DecodedValue> values_;
};

/**
 * Absent optional values leave the last observed value unchanged.
 * @param cursor Ordered decoded scalar rows.
 * @param next Snapshot receiving the six root fields.
 * @return False for a missing or invalid scalar.
 */
bool merge_scalars(Cursor& cursor, State& next) noexcept {
    for (std::uint16_t index = 0; index < kScalarCount; ++index) {
        const auto kind = index == kRealField ? ValueKind::real32 : ValueKind::signedInteger;
        const auto* value = cursor.take(kSchema, index, 0, kind, kScalarWidths[index], true);
        if (value == nullptr) {
            return false;
        }
        if (!value->present) {
            continue;
        }
        if (value->unsignedValue >= (std::uint64_t{1} << kScalarWidths[index])) {
            return false;
        }
        next.scalars[index] = {static_cast<std::uint32_t>(value->unsignedValue), true};
    }
    return true;
}

/**
 * The required fields accompany every nonempty squad delta.
 * @param cursor Ordered decoded scalar rows.
 * @param next Snapshot receiving required fields.
 * @return False for a missing or invalid field.
 */
bool merge_required(Cursor& cursor, State& next) noexcept {
    const auto* field6 = cursor.take(kSchema, kField6, 0, ValueKind::signedInteger, 2);
    const auto* field7 = cursor.take(kSchema, kField7, 0, ValueKind::signedInteger, 3);
    const auto* field8 = cursor.take(kSchema, kField8, 0, ValueKind::boolean, 1);
    const auto* field9 = cursor.take(kSchema, kField9, 0, ValueKind::boolean, 1);
    const auto* initialized = cursor.take(kSchema, kInitializedField, 0, ValueKind::boolean, 1);
    if (field6 == nullptr || field7 == nullptr || field8 == nullptr || field9 == nullptr
        || initialized == nullptr) {
        return false;
    }
    if (field6->signedValue < -1 || field6->signedValue > 2 || field7->signedValue < -1
        || field7->signedValue > 6 || field8->unsignedValue > 1 || field9->unsignedValue > 1
        || initialized->unsignedValue > 1) {
        return false;
    }
    next.field6 = static_cast<std::int8_t>(field6->signedValue);
    next.field7 = static_cast<std::int8_t>(field7->signedValue);
    next.field8 = field8->unsignedValue != 0;
    next.field9 = field9->unsignedValue != 0;
    next.initialized = initialized->unsignedValue != 0;
    return true;
}

/**
 * A shorter list removes its old tail; absent nested fields retain their snapshot values.
 * @param cursor Ordered decoded scalar rows.
 * @param next Snapshot receiving list and real changes.
 * @return False for incomplete arrays or unsafe counts.
 */
bool merge_nested(Cursor& cursor, State& next) noexcept {
    if (cursor.next_schema(kListSchema)) {
        const auto* count = cursor.take(kListSchema, 0, 0, ValueKind::unsignedInteger, kCountWidth);
        if (count == nullptr || count->unsignedValue > kCountCapacity) {
            return false;
        }
        next.count = static_cast<std::uint8_t>(count->unsignedValue);
        next.countsPresent = true;
        next.counts.fill(0);
        for (std::uint32_t index = 0; index < next.count; ++index) {
            const auto* value =
                cursor.take(kCountSchema, 0, index, ValueKind::signedInteger, kSignedWidth);
            if (value == nullptr || value->signedValue < (std::numeric_limits<std::int32_t>::min)()
                || value->signedValue > (std::numeric_limits<std::int32_t>::max)()) {
                return false;
            }
            next.counts[index] = static_cast<std::int32_t>(value->signedValue);
        }
    }
    if (cursor.next_schema(kRealSchema)) {
        next.realsPresent = true;
        for (std::uint32_t index = 0; index < kRealCount; ++index) {
            const auto* value =
                cursor.take(kRealSchema, 0, index, ValueKind::real32, kRealWidth, true);
            if (value == nullptr) {
                return false;
            }
            if (!value->present) {
                continue;
            }
            if (value->unsignedValue >= (std::uint64_t{1} << kRealWidth)) {
                return false;
            }
            next.reals[index] = {static_cast<std::uint32_t>(value->unsignedValue), true};
        }
    }
    return cursor.empty();
}
} // namespace

/**
 * The initialized bit clears stale state before another snapshot can be published.
 * @param state Persistent squad snapshot; unchanged on invalid input.
 * @param object Complete decoded squad object with its wire counter.
 * @param values Full packet value span indexed by the object.
 * @return True when the delta was accepted, including an invalidating delta.
 */
bool merge(State& state,
           const sense_update::DecodedObject& object,
           std::span<const DecodedValue> values) noexcept {
    if (object.status != sense_update::ObjectStatus::decoded || object.senseSchema != kSchema
        || !object.hasGeneration || object.firstValue > values.size()
        || object.valueCount > values.size() - object.firstValue) {
        return false;
    }
    if (object.deltaBits == 1 && object.valueCount == 0) {
        state.counter = object.generationPlusOne;
        return true;
    }
    Cursor cursor(values.subspan(object.firstValue, object.valueCount));
    State next = state;
    if (!merge_scalars(cursor, next) || !merge_required(cursor, next) || !merge_nested(cursor, next)
        || cursor.bits != object.deltaBits) {
        return false;
    }
    if (!next.initialized) {
        next = {};
    } else {
        next.valid = true;
    }
    next.counter = object.generationPlusOne;
    state = next;
    return true;
}

/**
 * A reset carries the full retained state with the original optional-field presence.
 * @param state Initialized snapshot to publish.
 * @param output Caller-owned body storage; contents unspecified on failure.
 * @param bytes Receives used bytes, or zero on failure.
 * @param bits Receives used bits, or zero on failure.
 * @return False for invalid state or insufficient output space.
 */
bool encode(const State& state,
            std::span<std::byte> output,
            std::size_t& bytes,
            std::size_t& bits) noexcept {
    bytes = 0;
    bits = 0;
    if (!state.valid || !state.initialized || state.count > kCountCapacity || state.field6 < -1
        || state.field6 > 2 || state.field7 < -1 || state.field7 > 6) {
        return false;
    }
    middleware::encoding::bits::Writer writer(output);
    if (!writer.write(1, 1)) {
        return false;
    }
    for (std::size_t index = 0; index < kScalarCount; ++index) {
        const auto& value = state.scalars[index];
        if (!writer.write(value.present, 1)) {
            return false;
        }
        if (value.present
            && (value.raw >= (std::uint64_t{1} << kScalarWidths[index])
                || !writer.write(value.raw, kScalarWidths[index]))) {
            return false;
        }
    }
    if (!writer.write(static_cast<std::uint32_t>(state.field6) + kByteBias, 2)
        || !writer.write(static_cast<std::uint32_t>(state.field7) + kByteBias, 3)
        || !writer.write(state.field8, 1) || !writer.write(state.field9, 1)
        || !writer.write(state.initialized, 1) || !writer.write(state.countsPresent, 1)) {
        return false;
    }
    if (state.countsPresent) {
        if (!writer.write(state.count, kCountWidth)) {
            return false;
        }
        for (std::size_t index = 0; index < state.count; ++index) {
            if (!writer.write(static_cast<std::uint32_t>(state.counts[index]) + kCountBias,
                              kSignedWidth)) {
                return false;
            }
        }
    }
    if (!writer.write(state.realsPresent, 1)) {
        return false;
    }
    if (state.realsPresent) {
        for (const auto& value : state.reals) {
            if (!writer.write(value.present, 1)) {
                return false;
            }
            if (value.present
                && (value.raw >= (std::uint64_t{1} << kRealWidth)
                    || !writer.write(value.raw, kRealWidth))) {
                return false;
            }
        }
    }
    if (!writer.finish(bytes)) {
        return false;
    }
    bits = writer.bit_count();
    return true;
}
} // namespace sunrise::middleware::bap::activity_message::squad_sense
