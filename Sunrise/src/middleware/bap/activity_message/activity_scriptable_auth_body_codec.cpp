#include <algorithm>
#include <bit>
#include <cmath>

#include "mission_auth_patch.h"
#include "scriptable_auth_internal.h"

// The two scriptable-auth bodies whose encoded width depends on the caller's values: the type-2
// actor channel set and keyed lanes, and the type-34 filter predicate list. Both select their
// child schema from a finite tag, and both write the same 55-bit lane ClientRef.

namespace sunrise::middleware::bap::activity_message::scriptable_auth {
namespace {

namespace bits = encoding::bits;

/** The lane ClientRef slot type is sent unsigned, biased so its absent value is zero. */
constexpr std::uint32_t kClientRefTypeBias = 1;
/** Highest 31-bit actor-control revision the client accepts. */
constexpr std::uint32_t kMaximumRevision = 0x7FFFFFFFU;

/** Accepts only the two actor-ownership branches whose effects are fully traced. */
[[nodiscard]] constexpr bool valid_type2_actor_binding(Type2ActorBinding binding) noexcept {
    return binding == Type2ActorBinding::selfOwned || binding == Type2ActorBinding::squadMember;
}

/** Converts the logical bias-one binding value to its three-bit wire value. */
[[nodiscard]] constexpr std::uint32_t type2_actor_binding_wire(Type2ActorBinding binding) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::int8_t>(binding) + 1);
}

/** Writes one exact 55-bit ClientRef without resolving or rewriting it. */
[[nodiscard]] bool write_lane_client_ref(bits::Writer& writer,
                                         const Type2LaneClientRef& reference) noexcept {
    if (reference.slotType < -1 || reference.slotType > 126) {
        return false;
    }
    const auto typeWire =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(reference.slotType)
                                   + static_cast<std::int32_t>(kClientRefTypeBias));
    const auto indexWire =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(reference.slotIndex)
                                   + static_cast<std::int32_t>(kClientRefIndexBias));
    return writer.write(reference.registryKey, kSigned32Width)
           && writer.write(typeWire, kClientRefTypeWidth)
           && writer.write(indexWire, kClientRefIndexWidth);
}

/** Writes the child selected by the first keyed-lane tag. */
[[nodiscard]] bool write_type2_lane_primary(bits::Writer& writer,
                                            const Type2LanePrimary& primary) noexcept {
    switch (primary.index()) {
    case 0: {
        const auto& value = std::get<0>(primary);
        return write_lane_client_ref(writer, value.reference) && writer.write(value.value, 8);
    }
    case 1:
        return writer.write(std::get<1>(primary).value, 32);
    case 2:
        return writer.write(std::bit_cast<std::uint32_t>(std::get<2>(primary).value), 32);
    case 3: {
        const auto& value = std::get<3>(primary);
        return write_lane_client_ref(writer, value.reference) && writer.write(value.value, 8)
               && writer.write(value.enabled ? 1U : 0U, 1);
    }
    case 4:
        return true;
    case 5:
        return writer.write(std::get<5>(primary).value, 6);
    case 6: {
        const auto& value = std::get<6>(primary);
        return writer.write(value.value, 32) && writer.write(value.enabled ? 1U : 0U, 1);
    }
    case 7: {
        const auto& value = std::get<7>(primary);
        return writer.write(value.value, 32)
               && writer.write(std::bit_cast<std::uint32_t>(value.real), 32);
    }
    case 8: {
        const auto& value = std::get<8>(primary);
        return write_lane_client_ref(writer, value.reference) && writer.write(value.value, 8);
    }
    case 9: {
        const auto& value = std::get<9>(primary);
        bool encoded = true;
        for (const std::uint32_t field : value.values) {
            encoded = encoded && writer.write(field, 32);
        }
        return encoded && write_lane_client_ref(writer, value.reference)
               && writer.write(static_cast<std::uint8_t>(value.mode + 1), 3)
               && writer.write(static_cast<std::uint8_t>(value.value + 128), 8);
    }
    default:
        return false;
    }
}

/** Child width of each of the ten primary lane schemas, in tag order. */
constexpr std::array<std::size_t, 10> kType2LanePrimaryBits{63, 32, 32, 64, 0, 6, 33, 64, 63, 162};
/** Highest lane index `.6.1` and `.6.2.0` can name in their six bits. */
constexpr std::uint8_t kType2AtomFieldMaximum = 0x3F;

/** @return Wire tag of the secondary schema, zero-based, where two is the quantized child. */
[[nodiscard]] std::size_t type2_lane_secondary(const Type2KeyedLane& lane) noexcept {
    if (lane.secondary.index() == 0) {
        return static_cast<std::size_t>(std::get<0>(lane.secondary));
    }
    return 2;
}

/** @return True when every value the selected lane children carry is representable. */
[[nodiscard]] bool valid_type2_lane(const Type2KeyedLane& lane) noexcept {
    return !((lane.secondary.index() == 0
              && std::get<0>(lane.secondary) != Type2LaneSecondaryEmpty::first
              && std::get<0>(lane.secondary) != Type2LaneSecondaryEmpty::second)
             || (lane.primary.index() == 2 && !std::isfinite(std::get<2>(lane.primary).value))
             || (lane.primary.index() == 5 && std::get<5>(lane.primary).value > 0x3FU)
             || (lane.primary.index() == 7 && !std::isfinite(std::get<7>(lane.primary).real))
             || (lane.primary.index() == 9
                 && (std::get<9>(lane.primary).mode < -1 || std::get<9>(lane.primary).mode > 6))
             || (lane.secondary.index() == 1 && std::get<1>(lane.secondary).value > 0x7FFU));
}

/** @return Meaningful width of one lane, both tags included. */
[[nodiscard]] std::size_t type2_lane_bit_count(const Type2KeyedLane& lane) noexcept {
    return 6U + kType2LanePrimaryBits[lane.primary.index()]
           + (type2_lane_secondary(lane) == 2 ? 11U : 0U);
}

/** Writes both lane tags and the selected children into an open writer. */
[[nodiscard]] bool write_type2_lane(bits::Writer& writer, const Type2KeyedLane& lane) noexcept {
    const std::size_t secondary = type2_lane_secondary(lane);
    bool encoded = writer.write(lane.primary.index() + 1U, 4) && writer.write(secondary + 1U, 2)
                   && write_type2_lane_primary(writer, lane.primary);
    if (encoded && secondary == 2) {
        encoded = writer.write(std::get<1>(lane.secondary).value, 11);
    }
    return encoded;
}

/** Reads one stored lane, consuming exactly the width its two tags select. */
[[nodiscard]] bool read_type2_lane(bits::Reader& reader) noexcept {
    std::uint64_t primaryTag = 0;
    std::uint64_t secondaryTag = 0;
    if (!reader.read(4U, primaryTag) || primaryTag == 0 || primaryTag > kType2LanePrimaryBits.size()
        || !reader.read(2U, secondaryTag) || secondaryTag == 0 || secondaryTag > 3) {
        return false;
    }
    std::size_t childBits = kType2LanePrimaryBits[primaryTag - 1U];
    std::uint64_t value = 0;
    while (childBits > 0) {
        const auto chunk = static_cast<std::uint8_t>(childBits > 32U ? 32U : childBits);
        if (!reader.read(chunk, value)) {
            return false;
        }
        childBits -= chunk;
    }
    return secondaryTag != 3 || (reader.read(11U, value) && value <= 0x7FFU);
}

/** @return True when every declared field of one complete type-2 root is representable. */
[[nodiscard]] bool valid_type2_body(const Type2Body& body) noexcept {
    const Type2ChannelState& state = body.channels;
    if (state.revision == 0 || state.revision > kMaximumRevision
        || !valid_type2_actor_binding(state.actorBinding) || state.count > state.channels.size()
        || state.temperamentCount > state.temperaments.size()) {
        return false;
    }
    for (std::size_t index = 0; index < state.temperamentCount; ++index) {
        const auto scanned = static_cast<std::ptrdiff_t>(index);
        if (std::find(state.temperaments.begin(),
                      state.temperaments.begin() + scanned,
                      state.temperaments[index])
            != state.temperaments.begin() + scanned) {
            return false;
        }
    }
    for (std::size_t index = 0; index < state.count; ++index) {
        if (!std::isfinite(state.channels[index].value)) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state.channels[prior].nameHash == state.channels[index].nameHash) {
                return false;
            }
        }
    }
    if (body.atoms.generation == 0) {
        return true;
    }
    if (body.atoms.generation > kMaximumRevision || body.atoms.count > body.atoms.lanes.size()
        || body.atoms.progressSeed > kType2AtomFieldMaximum
        || body.atoms.progressSeed > body.atoms.count) {
        return false;
    }
    for (std::size_t index = 0; index < body.atoms.count; ++index) {
        if (!valid_type2_lane(body.atoms.lanes[index])) {
            return false;
        }
    }
    return true;
}

/** @return Meaningful width of one complete type-2 root. */
[[nodiscard]] std::size_t type2_body_bit_count(const Type2Body& body) noexcept {
    std::size_t bitCount = kType2ChannelBaseBitCount
                           + body.channels.temperamentCount * kSigned32Width
                           + body.channels.count * kType2ChannelRowBitCount;
    if (body.atoms.generation != 0) {
        bitCount += kType2AtomBlockBitCount;
        for (std::size_t index = 0; index < body.atoms.count; ++index) {
            bitCount += 1U + type2_lane_bit_count(body.atoms.lanes[index]);
        }
    }
    return bitCount;
}

/** Writes the whole root: `.0`, `.4` and `.7` absent, the control block, then the atom program. */
[[nodiscard]] bool write_type2_root(bits::Writer& writer, const Type2Body& body) noexcept {
    const Type2ChannelState& state = body.channels;
    bool encoded = writer.write(0, kBoolWidth) && writer.write(0, 2U)
                   && writer.write(type2_actor_binding_wire(state.actorBinding), 3U)
                   && writer.write(kEnabled, kBoolWidth) && writer.write(0, kBoolWidth)
                   && writer.write(1, kBoolWidth) && writer.write(state.revision, 31U)
                   && writer.write(0, 6U) && writer.write(0, 6U)
                   && writer.write(state.temperamentCount, 3U);
    for (std::size_t index = 0; encoded && index < state.temperamentCount; ++index) {
        encoded = writer.write(state.temperaments[index].value, kSigned32Width);
    }
    encoded = encoded && write_absent_client_ref(writer) && writer.write(0, kSigned32Width)
              && writer.write(state.count, 5U);
    for (std::size_t index = 0; encoded && index < state.count; ++index) {
        encoded = writer.write(state.channels[index].nameHash, kSigned32Width)
                  && writer.write(std::bit_cast<std::uint32_t>(state.channels[index].value),
                                  kReal32Width);
    }
    const bool atoms = body.atoms.generation != 0;
    encoded = encoded && writer.write(atoms ? kEnabled : 0U, kBoolWidth);
    if (atoms) {
        encoded = encoded && writer.write(body.atoms.generation, 31U)
                  && writer.write(body.atoms.progressSeed, 6U)
                  && writer.write(body.atoms.count, 6U);
        for (std::size_t index = 0; encoded && index < body.atoms.count; ++index) {
            encoded = writer.write(kEnabled, kBoolWidth)
                      && write_type2_lane(writer, body.atoms.lanes[index]);
        }
    }
    return encoded && writer.write(0, kBoolWidth);
}

/** Child schema the client selects for each type-34 predicate tag, in tag order. */
constexpr std::array<std::uint32_t, 13> kType34PredicateSchemas{
    0x80809571U,
    0x80809572U,
    0x80809573U,
    0x80809574U,
    0x80809575U,
    0x80809576U,
    0x80809577U,
    0x80809578U,
    0x80809579U,
    0x8080957AU,
    0x8080957BU,
    0x8080957CU,
    0x8080957DU,
};

/** Bits each predicate child body occupies, in the same tag order. */
constexpr std::array<std::size_t, 13> kType34PredicateChildBits{
    34,
    34,
    2,
    57,
    34,
    58,
    57,
    57,
    57,
    34,
    57,
    34,
    2,
};

template <typename Value>
[[nodiscard]] bool write_type34_mode(bits::Writer& writer, const Value& value) noexcept {
    return valid_mode(value.mode)
           && writer.write(static_cast<std::uint8_t>(value.mode + kModeBias), kModeWidth);
}

template <typename Value>
[[nodiscard]] bool write_type34_mode_u32(bits::Writer& writer, const Value& value) noexcept {
    return write_type34_mode(writer, value) && writer.write(value.value, kSigned32Width);
}

template <typename Value>
[[nodiscard]] bool write_type34_mode_ref(bits::Writer& writer, const Value& value) noexcept {
    return write_type34_mode(writer, value) && write_lane_client_ref(writer, value.reference);
}

/** Writes the predicate child body selected by the variant's own tag. */
[[nodiscard]] bool write_type34_child(bits::Writer& writer,
                                      const Type34Predicate& predicate) noexcept {
    switch (predicate.index()) {
    case 0:
        return write_type34_mode_u32(writer, std::get<0>(predicate));
    case 1:
        return write_type34_mode_u32(writer, std::get<1>(predicate));
    case 2:
        return write_type34_mode(writer, std::get<2>(predicate));
    case 3:
        return write_type34_mode_ref(writer, std::get<3>(predicate));
    case 4:
        return write_type34_mode_u32(writer, std::get<4>(predicate));
    case 5: {
        const auto& value = std::get<5>(predicate);
        return write_type34_mode(writer, value) && writer.write(value.flag ? 1U : 0U, kBoolWidth)
               && write_lane_client_ref(writer, value.reference);
    }
    case 6:
        return write_type34_mode_ref(writer, std::get<6>(predicate));
    case 7:
        return write_type34_mode_ref(writer, std::get<7>(predicate));
    case 8:
        return write_type34_mode_ref(writer, std::get<8>(predicate));
    case 9: {
        const auto& value = std::get<9>(predicate);
        return write_type34_mode(writer, value)
               && writer.write(std::bit_cast<std::uint32_t>(value.value) + kSigned32Bias,
                               kSigned32Width);
    }
    case 10:
        return write_type34_mode_ref(writer, std::get<10>(predicate));
    case 11:
        return write_type34_mode_u32(writer, std::get<11>(predicate));
    case 12:
        return write_type34_mode(writer, std::get<12>(predicate));
    default:
        return false;
    }
}

/** Writes one present predicate: its presence bit, schema handle, and child body. */
[[nodiscard]] bool write_type34_predicate(bits::Writer& writer,
                                          const Type34Predicate& predicate) noexcept {
    const std::size_t selector = predicate.index();
    return selector < kType34PredicateSchemas.size() && writer.write(1, kBoolWidth)
           && writer.write(kType34PredicateSchemas[selector], kSigned32Width)
           && write_type34_child(writer, predicate);
}

} // namespace

/** Updates one named actor channel while preserving every other retained row. */
bool set_type2_channel(Type2ChannelState& state, std::uint32_t nameHash, float value) noexcept {
    if (state.count > state.channels.size() || !std::isfinite(value)) {
        return false;
    }
    for (std::size_t index = 0; index < state.count; ++index) {
        if (state.channels[index].nameHash == nameHash) {
            state.channels[index].value = value;
            return true;
        }
    }
    if (state.count >= state.channels.size()) {
        return false;
    }
    state.channels[state.count++] = {nameHash, value};
    return true;
}

/** Finds the next 31-bit actor-control revision without wrapping. */
bool next_type2_revision(const Type2ChannelState& state, std::uint32_t& next) noexcept {
    next = 0;
    if (state.revision >= kMaximumRevision) {
        return false;
    }
    next = state.revision + 1U;
    return next != 0;
}

bool set_type2_actor_binding(Type2ChannelState& state, Type2ActorBinding binding) noexcept {
    if (!valid_type2_actor_binding(binding)) {
        return false;
    }
    state.actorBinding = binding;
    return true;
}

/** Replaces the retained temperament list, refusing a repeated identity. */
bool set_type2_temperaments(Type2ChannelState& state,
                            std::span<const TemperamentId> temperaments) noexcept {
    if (temperaments.size() > state.temperaments.size()) {
        return false;
    }
    std::array<TemperamentId, kType2TemperamentCapacity> retained{};
    for (std::size_t index = 0; index < temperaments.size(); ++index) {
        const auto scanned = static_cast<std::ptrdiff_t>(index);
        if (std::find(retained.begin(), retained.begin() + scanned, temperaments[index])
            != retained.begin() + scanned) {
            return false;
        }
        retained[index] = temperaments[index];
    }
    state.temperaments = retained;
    state.temperamentCount = static_cast<std::uint8_t>(temperaments.size());
    return true;
}

/**
 * Encodes the finite type-38 reflection union used by one type-2 keyed lane.
 * @param lane Both selected tags and their child values.
 * @param output Caller storage, left unchanged when the lane is refused.
 * @param written Receives the encoded byte count.
 * @param writtenBits Receives the meaningful bit count before byte padding.
 * @return True when the lane encoded.
 */
bool encode_type2_keyed_lane(const Type2KeyedLane& lane,
                             std::span<std::byte> output,
                             std::size_t& written,
                             std::size_t& writtenBits) noexcept {
    written = 0;
    writtenBits = 0;
    if (!valid_type2_lane(lane)) {
        return false;
    }
    const std::size_t bitCount = type2_lane_bit_count(lane);
    const std::size_t byteCount = (bitCount + 7U) / 8U;
    if (output.size() < byteCount) {
        return false;
    }
    bits::Writer writer(output.first(byteCount));
    if (!write_type2_lane(writer, lane) || writer.bit_count() != bitCount || !writer.finish(written)
        || written != byteCount) {
        written = 0;
        return false;
    }
    writtenBits = bitCount;
    return true;
}

/**
 * Encodes one present code-34 field: presence, schema handle, and its native child body.
 * @param predicate Selected predicate schema and its values.
 * @param output Caller storage, left unchanged when the predicate is refused.
 * @param written Receives the encoded byte count.
 * @param writtenBits Receives the meaningful bit count before byte padding.
 * @return True when the predicate encoded.
 */
bool encode_type34_predicate(const Type34Predicate& predicate,
                             std::span<std::byte> output,
                             std::size_t& written,
                             std::size_t& writtenBits) noexcept {
    written = 0;
    writtenBits = 0;
    const std::size_t selector = predicate.index();
    if (selector >= kType34PredicateChildBits.size()) {
        return false;
    }
    writtenBits = 33U + kType34PredicateChildBits[selector];
    const std::size_t byteCount = (writtenBits + 7U) / 8U;
    if (output.size() < byteCount) {
        writtenBits = 0;
        return false;
    }
    bits::Writer writer(output.first(byteCount));
    if (!write_type34_predicate(writer, predicate) || writer.bit_count() != writtenBits
        || !writer.finish(written) || written != byteCount) {
        written = 0;
        writtenBits = 0;
        return false;
    }
    return true;
}

/**
 * Encodes the complete type-34 body containing zero to eight predicates.
 * @param body Predicate list and its count.
 * @param output Caller storage, left unchanged when the body is refused.
 * @param written Receives the encoded byte count.
 * @param writtenBits Receives the meaningful bit count before byte padding.
 * @return True when the body encoded.
 */
bool encode_type34(const Type34Body& body,
                   std::span<std::byte> output,
                   std::size_t& written,
                   std::size_t& writtenBits) noexcept {
    written = 0;
    writtenBits = 0;
    if (body.count > body.predicates.size()) {
        return false;
    }
    std::size_t bitCount = 4U;
    for (std::size_t index = 0; index < body.count; ++index) {
        bitCount += 33U + kType34PredicateChildBits[body.predicates[index].index()];
    }
    const std::size_t byteCount = (bitCount + 7U) / 8U;
    if (output.size() < byteCount) {
        return false;
    }
    bits::Writer writer(output.first(byteCount));
    bool encoded = writer.write(body.count, 4U);
    for (std::size_t index = 0; encoded && index < body.count; ++index) {
        encoded = write_type34_predicate(writer, body.predicates[index]);
    }
    if (!encoded || writer.bit_count() != bitCount || !writer.finish(written)
        || written != byteCount) {
        written = 0;
        return false;
    }
    writtenBits = bitCount;
    return true;
}

/** Writes only the type-2 actor-channel block; every unrelated optional root stays absent. */
bool encode_type2_channels(const Type2ChannelState& state,
                           std::span<std::byte> output,
                           std::size_t& written,
                           std::size_t& writtenBits) noexcept {
    return encode_type2_body(Type2Body{state, {}}, output, written, writtenBits);
}

/**
 * Writes the complete type-2 root. `.0`, `.4` and `.7` stay absent, `.5` always writes, and `.6`
 * writes only for a nonzero atom generation.
 * @param body Control block and atom program to encode together.
 * @param output Caller storage, left unchanged when the body is refused.
 * @param written Receives the encoded byte count.
 * @param writtenBits Receives the meaningful bit count before byte padding.
 * @return True when the body encoded.
 */
bool encode_type2_body(const Type2Body& body,
                       std::span<std::byte> output,
                       std::size_t& written,
                       std::size_t& writtenBits) noexcept {
    written = 0;
    writtenBits = 0;
    if (!valid_type2_body(body)) {
        return false;
    }
    const std::size_t bitCount = type2_body_bit_count(body);
    const std::size_t byteCount = (bitCount + 7U) / 8U;
    if (output.size() < byteCount) {
        return false;
    }
    bits::Writer writer(output.first(byteCount));
    if (!write_type2_root(writer, body) || writer.bit_count() != bitCount || !writer.finish(written)
        || written != byteCount) {
        written = 0;
        return false;
    }
    writtenBits = bitCount;
    return true;
}

/** Reads every optional root block before reporting the atom program span. */
bool inspect_type2_program(std::span<const std::byte> input,
                           std::size_t bitCount,
                           Type2ProgramLayout& output) noexcept {
    output = {};
    if (bitCount < 11U || bitCount > kType2FullMaximumBitCount
        || input.size() != (bitCount + 7U) / 8U) {
        return false;
    }
    bits::Reader reader(input);
    const auto optional = [&](std::size_t width) noexcept {
        std::uint64_t present = 0;
        return reader.read(1, present) && (present == 0 || reader.skip(width));
    };
    std::uint64_t value = 0, present = 0, count = 0;
    Type2ProgramLayout candidate{};
    if (!optional(31) || !reader.skip(2) || !reader.read(3, value)) {
        return false;
    }
    candidate.bindingWire = static_cast<std::uint8_t>(value);
    if (!reader.read(1, value)) {
        return false;
    }
    candidate.enabled = value != 0;
    if (!reader.read(1, present)) {
        return false;
    }
    if (present != 0) {
        if (!reader.read(1, present)) {
            return false;
        }
        if (present != 0) {
            for (std::size_t index = 0; index < 8; ++index) {
                if (!optional(32) || !optional(31)) {
                    return false;
                }
            }
        }
        if (!optional(32)) {
            return false;
        }
    }
    if (!reader.read(1, present)) {
        return false;
    }
    if (present != 0) {
        if (!reader.skip(31 + 6 + 6) || !reader.read(3, count) || count > kType2TemperamentCapacity
            || !reader.skip(count * 32) || !reader.skip(55 + 32) || !reader.read(5, count)
            || count > kType2ChannelCapacity || !reader.skip(count * 64)) {
            return false;
        }
    }
    candidate.programOffset = input.size() * 8U - reader.remaining_bits();
    if (!reader.read(1, present)) {
        return false;
    }
    if (present != 0) {
        std::uint64_t progress = 0;
        if (!reader.read(31, value) || !reader.read(6, progress) || !reader.read(6, count)
            || count > kType2AtomCapacity || progress > count) {
            return false;
        }
        candidate.generation = static_cast<std::uint32_t>(value);
        for (std::size_t index = 0; index < count; ++index) {
            if (!reader.read(1, present) || (present != 0 && !read_type2_lane(reader))) {
                return false;
            }
        }
    }
    candidate.programBits = input.size() * 8U - reader.remaining_bits() - candidate.programOffset;
    if (!reader.read(1, present)) {
        return false;
    }
    if (present != 0 && (!reader.read(4, count) || count > 8 || !reader.skip(count * 55 + 31))) {
        return false;
    }
    if (reader.remaining_bits() != input.size() * 8U - bitCount || !finish_padding(reader)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Preserves all root fields except the new sequence program and its generation. */
bool replace_type2_sequence(std::span<const std::byte> previous,
                            std::size_t previousBits,
                            std::uint32_t sequenceHash,
                            std::uint32_t committedGeneration,
                            std::span<std::byte> output,
                            std::size_t& written,
                            std::size_t& writtenBits,
                            std::uint32_t& generation) noexcept {
    written = 0;
    writtenBits = 0;
    generation = 0;
    Type2ProgramLayout layout{};
    if (sequenceHash == kClientRefAbsentKey
        || sequenceHash == (std::numeric_limits<std::uint32_t>::max)()
        || !inspect_type2_program(previous, previousBits, layout) || !layout.enabled
        || (layout.bindingWire != 2 && layout.bindingWire != 4)) {
        return false;
    }
    const auto last = (std::max)(committedGeneration, layout.generation);
    if (last >= kMaximumRevision) {
        return false;
    }
    const auto next = last + 1;
    const bool play = sequenceHash != 0;
    Type2KeyedLane lane{};
    lane.primary = Type2LaneU32{sequenceHash};
    lane.secondary = Type2LaneSecondaryEmpty::first;
    std::array<std::byte, kType2FullMaximumByteCount> staged{};
    bits::Writer writer(staged);
    namespace patch = mission_auth_patch;
    const auto tailOffset = layout.programOffset + layout.programBits;
    if (!patch::copy_field(writer, previous, {0, layout.programOffset, true}) || !writer.write(1, 1)
        || !writer.write(next, 31) || !writer.write(0, 6) || !writer.write(play ? 1 : 0, 6)
        || (play && (!writer.write(1, 1) || !write_type2_lane(writer, lane)))
        || !patch::copy_field(writer, previous, {tailOffset, previousBits - tailOffset, true})) {
        return false;
    }
    const auto bitCount = writer.bit_count();
    std::size_t byteCount = 0;
    if (!writer.finish(byteCount) || byteCount > output.size()) {
        return false;
    }
    std::copy_n(staged.begin(), byteCount, output.begin());
    written = byteCount;
    writtenBits = bitCount;
    generation = next;
    return true;
}

/** Accepts only a canonical type-2 root: the actor-control block and an optional atom program. */
bool validate_type2_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    if (bitCount < kType2ChannelBaseBitCount || bitCount > kType2MaximumBodyBitCount
        || input.size() != (bitCount + 7U) / 8U) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t value = 0;
    std::uint64_t revision = 0;
    std::uint64_t temperamentCount = 0;
    std::uint64_t channelCount = 0;
    if (!reader.read(1U, value) || value != 0 || !reader.read(2U, value) || value != 0
        || !reader.read(3U, value) || (value != 0 && value != 2) || !reader.read(1U, value)
        || value != 1 || !reader.read(1U, value) || value != 0 || !reader.read(1U, value)
        || value != 1 || !reader.read(31U, revision) || revision == 0 || !reader.read(6U, value)
        || value != 0 || !reader.read(6U, value) || value != 0 || !reader.read(3U, temperamentCount)
        || temperamentCount > kType2TemperamentCapacity) {
        return false;
    }
    std::array<std::uint32_t, kType2TemperamentCapacity> temperaments{};
    for (std::size_t index = 0; index < temperamentCount; ++index) {
        if (!reader.read(32U, value)) {
            return false;
        }
        temperaments[index] = static_cast<std::uint32_t>(value);
        const auto scanned = static_cast<std::ptrdiff_t>(index);
        if (std::find(temperaments.begin(), temperaments.begin() + scanned, temperaments[index])
            != temperaments.begin() + scanned) {
            return false;
        }
    }
    if (!read_absent_client_ref(reader) || !reader.read(32U, value) || value != 0
        || !reader.read(5U, channelCount) || channelCount > kType2ChannelCapacity) {
        return false;
    }
    std::array<std::uint32_t, kType2ChannelCapacity> channels{};
    for (std::size_t index = 0; index < channelCount; ++index) {
        std::uint64_t real = 0;
        if (!reader.read(32U, value) || !reader.read(32U, real)
            || !std::isfinite(std::bit_cast<float>(static_cast<std::uint32_t>(real)))) {
            return false;
        }
        channels[index] = static_cast<std::uint32_t>(value);
        const auto scanned = static_cast<std::ptrdiff_t>(index);
        if (std::find(channels.begin(), channels.begin() + scanned, channels[index])
            != channels.begin() + scanned) {
            return false;
        }
    }
    std::uint64_t atomsPresent = 0;
    if (!reader.read(1U, atomsPresent)) {
        return false;
    }
    if (atomsPresent != 0) {
        std::uint64_t generation = 0;
        std::uint64_t progressSeed = 0;
        std::uint64_t atomCount = 0;
        if (!reader.read(31U, generation) || generation == 0 || !reader.read(6U, progressSeed)
            || !reader.read(6U, atomCount) || atomCount > kType2AtomCapacity
            || progressSeed > atomCount) {
            return false;
        }
        for (std::uint64_t index = 0; index < atomCount; ++index) {
            if (!reader.read(1U, value) || value != 1 || !read_type2_lane(reader)) {
                return false;
            }
        }
    }
    return reader.read(1U, value) && value == 0
           && reader.remaining_bits() == input.size() * 8U - bitCount && finish_padding(reader);
}

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
