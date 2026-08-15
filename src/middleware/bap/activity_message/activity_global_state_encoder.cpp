#include "activity_global_state_encoder.h"

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"

namespace sunrise::middleware::bap::activity_message::global_activity_state {
namespace {

/** The widest bias-1 scalar a descriptor index field accepts. */
constexpr std::int32_t kIndexMaximum = (1 << kDescriptorIndexWidth) - 2;
/** The widest bias-1 scalar the reason field accepts. */
constexpr std::int32_t kReasonMaximum = (1 << kDescriptorReasonWidth) - 2;
/** The widest slice-set index the bias-1 field accepts. */
constexpr std::uint32_t kSliceSetMaximum = (1U << kSliceSetWidth) - 2U;
/** The replay moves this many bits per step, which is the writer's widest single field. */
constexpr std::uint8_t kMaximumReplayWidth = 64;
/** Bits in one byte, the stride of the captured descriptor's storage. */
constexpr std::size_t kBitsPerByte = 8;
/**
 * Width of the configured minimal descriptor. The minimum encoding is sized for it. The client's
 * own descriptor is wider whenever it carries an optional field, so the body grows with it
 * instead of being clamped to the minimum.
 */
constexpr std::size_t kConfiguredDescriptorBits = kMeaningfulBitCount - kDescriptorBit - kTailBits;
static_assert(kConfiguredDescriptorBits == 372);

/**
 * Sizes the body for one descriptor width.
 * @param descriptorBits Bits the descriptor takes.
 * @return Whole bytes the body needs, never below the minimum encoding.
 */
[[nodiscard]] constexpr std::size_t body_size(std::size_t descriptorBits) noexcept {
    const std::size_t bits = kDescriptorBit + descriptorBits + kTailBits;
    const std::size_t bytes = (bits + kBitsPerByte - 1) / kBitsPerByte;
    return bytes < kEncodedSize ? kEncodedSize : bytes;
}

static_assert(body_size(kConfiguredDescriptorBits) == kEncodedSize);

/**
 * Pads the writer with zeroes up to one absolute bit position.
 * @param target Absolute bit position to reach.
 * @return True when the writer was at or before that position and the padding fitted.
 */
[[nodiscard]] bool pad_to(encoding::bits::Writer& writer, std::size_t target) noexcept {
    if (writer.bit_count() > target) {
        return false;
    }
    while (writer.bit_count() < target) {
        const std::size_t remaining = target - writer.bit_count();
        const auto width = static_cast<std::uint8_t>(remaining > 32 ? 32 : remaining);
        if (!writer.write(0, width)) {
            return false;
        }
    }
    return true;
}

/**
 * Encodes one bias-1 selection scalar.
 * @param value Scalar, where -1 means absent.
 * @param maximum Widest value the field accepts.
 * @param encoded Receives the biased value.
 * @return True when the scalar fits the field.
 */
[[nodiscard]] bool
selection_scalar(std::int32_t value, std::int32_t maximum, std::uint32_t& encoded) noexcept {
    if (value < -1 || value > maximum) {
        return false;
    }
    encoded = static_cast<std::uint32_t>(value + 1);
    return true;
}

/**
 * Replays the client's own descriptor bits into the body. Both sides are most-significant-bit
 * first, so the copy moves whole words and one partial tail.
 * @param writer Body writer sitting at the descriptor.
 * @param state Body input holding the captured bits and their length.
 * @return True when every captured bit was written.
 */
[[nodiscard]] bool replay_descriptor(encoding::bits::Writer& writer,
                                     const GlobalActivityState& state) noexcept {
    if (state.descriptorBitLength > state.descriptorBits.size() * kBitsPerByte) {
        return false;
    }
    encoding::bits::Reader reader(state.descriptorBits);
    std::size_t remaining = state.descriptorBitLength;
    while (remaining != 0) {
        const auto width = static_cast<std::uint8_t>(
            remaining > kMaximumReplayWidth ? kMaximumReplayWidth : remaining);
        std::uint64_t value = 0;
        if (!reader.read(width, value) || !writer.write(value, width)) {
            return false;
        }
        remaining -= width;
    }
    return true;
}

/**
 * Writes the minimal selection descriptor, including the name.
 * @param writer Body writer sitting at the descriptor bit.
 * @return True when the whole descriptor was written.
 */
[[nodiscard]] bool write_descriptor(encoding::bits::Writer& writer,
                                    const GlobalActivityState& state) noexcept {
    std::uint32_t reason = 0;
    std::uint32_t from = 0;
    std::uint32_t activity = 0;
    if (!selection_scalar(state.reason, kReasonMaximum, reason)
        || !selection_scalar(state.fromActivityIndex, kIndexMaximum, from)
        || !selection_scalar(state.activityIndex, kIndexMaximum, activity)) {
        return false;
    }
    if (!writer.write(reason, kDescriptorReasonWidth) || !writer.write(from, kDescriptorIndexWidth)
        || !writer.write(activity, kDescriptorIndexWidth)) {
        return false;
    }
    // Element index, account soid and nonce are absent, then the skull count and one spare field.
    if (!writer.write(0, 3) || !writer.write(0, 5) || !writer.write(0, 8)) {
        return false;
    }
    // Two spares, the arrival bubble hash and the spawn set hash are absent. The name is present.
    if (!writer.write(0, 3) || !writer.write(1, 1)) {
        return false;
    }
    if (writer.bit_count() != kNameBit) {
        return false;
    }
    for (std::size_t index = 0; index < kNameCapacity; ++index) {
        // The last element is kept back, so a full-length name still ends with a biased zero.
        const bool inName = index < state.nameLength && index + 1 < kNameCapacity;
        const auto character =
            inName ? static_cast<std::uint8_t>(state.name[index]) : std::uint8_t{};
        if (!writer.write(static_cast<std::uint8_t>(character + kNameBias), 8)) {
            return false;
        }
    }
    return writer.write(0, 4);
}

} // namespace

/** Encodes one global activity state body with the configured selection descriptor. */
bool encode_global_activity_state(const GlobalActivityState& state,
                                  std::span<std::byte> output,
                                  std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kEncodedSize || state.nameLength >= kNameCapacity
        || state.bubbleCount > kBubbleStateCount) {
        return false;
    }
    if (state.hasSliceSet && state.sliceSetIndex > kSliceSetMaximum) {
        return false;
    }
    // The client's own descriptor is replayed when it was captured, because it carries fields with
    // no known name. It is wider than the configured minimal form whenever it names an optional
    // field, so the body is sized for the descriptor in hand, not for the minimum.
    const bool replayed = state.descriptorBitLength != 0;
    const std::size_t descriptorBits =
        replayed ? state.descriptorBitLength : kConfiguredDescriptorBits;
    const std::size_t bodySize = body_size(descriptorBits);
    if (output.size() < bodySize
        || (replayed && state.descriptorBitLength > state.descriptorBits.size() * kBitsPerByte)) {
        return false;
    }

    encoding::bits::Writer writer(output.first(bodySize));
    // The arm bit is the last of six sources the client tries, so setting it disturbs nothing.
    if (!writer.write(1, 1) || !pad_to(writer, kBubbleCountBit)) {
        return false;
    }
    if (!writer.write(kBubbleCountBias + state.bubbleCount, kBubbleCountWidth)) {
        return false;
    }
    if (writer.bit_count() != kBubbleStateBit) {
        return false;
    }
    for (std::size_t index = 0; index < kBubbleStateCount; ++index) {
        // Past the declared count the elements are never read, but must still be encoded.
        const std::uint8_t value =
            index < state.bubbleCount ? state.bubbleStates[index] : kBubbleStateNone;
        if (!writer.write(value, 8)) {
            return false;
        }
    }
    if (!pad_to(writer, kSliceSetBit)) {
        return false;
    }
    const std::uint32_t sliceSet = state.hasSliceSet ? state.sliceSetIndex + kSliceSetBias : 0U;
    if (!writer.write(sliceSet, kSliceSetWidth)) {
        return false;
    }
    if (writer.bit_count() != kSpawnSetBit || !writer.write(state.spawnSetHash, kSpawnSetWidth)) {
        return false;
    }
    if (!pad_to(writer, kDescriptorBit)) {
        return false;
    }
    if (replayed ? !replay_descriptor(writer, state) : !write_descriptor(writer, state)) {
        return false;
    }
    // The tail is a fixed width after the descriptor, so a descriptor of another length moves the
    // body's end instead of overrunning a fixed total.
    if (!pad_to(writer, kDescriptorBit + descriptorBits + kTailBits)) {
        return false;
    }

    std::size_t produced = 0;
    if (!writer.finish(produced) || produced != bodySize) {
        return false;
    }
    written = produced;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::global_activity_state
