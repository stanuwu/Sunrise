#include "external_entity_codec.h"

#include <array>

namespace sunrise::middleware::gameplay::external {
namespace {

/** Presence, count, shortcut, and trailing fields use one bit. */
constexpr std::uint8_t kFlagWidth = 1;
/** The wire token carries a 13-bit slot. */
constexpr std::uint8_t kEntitySlotWidth = 13;
/** The wire token carries a four-bit incarnation. */
constexpr std::uint8_t kEntityIncarnationWidth = 4;
/** A raw bubble that fits on wire uses one byte. */
constexpr std::uint8_t kRawBubbleWidth = 8;
/** The optional raw-bubble value is limited to one unsigned byte. */
constexpr std::uint16_t kMaximumRawBubble = 0xFF;
/** A strict remove declares its one-bit body in a signed 16-bit field. */
constexpr std::uint8_t kSubrecordLengthWidth = 16;
/** A create carries one lifecycle byte. */
constexpr std::uint8_t kLifecycleRevisionWidth = 8;
/** A create carries one two-bit entity type. */
constexpr std::uint8_t kEntityTypeWidth = 2;
/** A strict administrative remove contains only its trailing false bit. */
constexpr std::uint16_t kStrictRemoveBodyBits = 1;
/** All flags represented by the explicit five-bit envelope. */
constexpr std::uint16_t kAllowedFlags =
    entityCreate | entityUpdate | entityRemove | entityLifecycle | entityAnchor;
/** Explicit flags are sent in this fixed order. */
constexpr std::array<std::uint16_t, 5> kExplicitFlags{
    entityCreate, entityUpdate, entityRemove, entityLifecycle, entityAnchor};

/** Measured callback lengths used to reject a changed second pass. */
struct PayloadPlan {
    std::size_t baselineBits{};
    std::size_t updateBits{};
};

/** Reads one boolean field. */
[[nodiscard]] bool read_flag(encoding::bits::Reader& reader, bool& output) noexcept {
    std::uint64_t value = 0;
    if (!reader.read(kFlagWidth, value)) {
        return false;
    }
    output = value != 0;
    return true;
}

/** Writes one boolean field. */
[[nodiscard]] bool write_flag(encoding::bits::Writer& writer, bool value) noexcept {
    return writer.write(value ? 1U : 0U, kFlagWidth);
}

/** @return True when a token fits its 17-bit wire identity. */
[[nodiscard]] bool valid_token(const EntityToken& token) noexcept {
    return token.slot <= kMaximumEntitySlot && token.incarnation <= kMaximumEntityIncarnation;
}

/** @return True when a raw bubble is absent or fits the optional byte. */
[[nodiscard]] bool valid_raw_bubble(std::uint16_t rawBubble) noexcept {
    return rawBubble == kNoRawBubble || rawBubble <= kMaximumRawBubble;
}

/** @return True when a value is one of the four wire entity types. */
[[nodiscard]] bool valid_type(EntityType type) noexcept {
    return type < EntityType::count;
}

/** @return True for create/update records or the exact administrative remove form. */
[[nodiscard]] bool valid_flags(std::uint16_t flags) noexcept {
    if ((flags & ~kAllowedFlags) != 0) {
        return false;
    }
    if (flags == entityRemove) {
        return true;
    }
    return (flags & entityRemove) == 0 && (flags & (entityCreate | entityUpdate)) != 0;
}

/** @return The declared callback limit for one type-payload part. */
[[nodiscard]] std::size_t payload_limit(const TypePayloadCodec& codec,
                                        TypePayloadPart part) noexcept {
    return part == TypePayloadPart::baseline ? codec.maximumBaselineBits : codec.maximumUpdateBits;
}

/** @return True when callback state and its declared bit limit are bounded. */
[[nodiscard]] bool valid_payload(const TypePayloadCodec& codec,
                                 TypePayloadPart part,
                                 const TypePayload& payload) noexcept {
    return payload.byteCount <= payload.state.size()
           && payload_limit(codec, part) <= kMaximumTypePayloadBits;
}

/** Validates the canonical record forms accepted by the generic envelope. */
[[nodiscard]] bool valid_record(const EntityRecord& record) noexcept {
    if (!valid_token(record.token) || !valid_raw_bubble(record.rawBubble)
        || !valid_flags(record.flags)) {
        return false;
    }
    if (record.flags == entityRemove) {
        return !record.anchorPresent && !record.trailingState && record.lifecycleRevision == 0
               && record.baseline.byteCount == 0 && record.update.byteCount == 0;
    }
    if (!valid_type(record.type) || (record.anchorPresent && (record.flags & entityAnchor) == 0)
        || ((record.flags & entityAnchor) != 0 && record.anchorPresent
            && !valid_token(record.anchor))
        || record.trailingState) {
        return false;
    }
    if ((record.flags & entityCreate) == 0
        && (record.lifecycleRevision != 0 || record.baseline.byteCount != 0)) {
        return false;
    }
    return (record.flags & entityUpdate) != 0 || record.update.byteCount == 0;
}

/** Writes one token in slot-then-incarnation order. */
[[nodiscard]] bool write_token(encoding::bits::Writer& writer, const EntityToken& token) noexcept {
    return valid_token(token) && writer.write(token.slot, kEntitySlotWidth)
           && writer.write(token.incarnation, kEntityIncarnationWidth);
}

/** Reads one token without changing output on failure. */
[[nodiscard]] bool read_token(encoding::bits::Reader& reader, EntityToken& output) noexcept {
    std::uint64_t slot = 0;
    std::uint64_t incarnation = 0;
    if (!reader.read(kEntitySlotWidth, slot)
        || !reader.read(kEntityIncarnationWidth, incarnation)) {
        return false;
    }
    output.slot = static_cast<std::uint16_t>(slot);
    output.incarnation = static_cast<std::uint8_t>(incarnation);
    return true;
}

/** Measures one pure payload callback before any envelope bits are written. */
[[nodiscard]] bool measure_payload(const TypePayloadCodec& codec,
                                   const EntityRecord& record,
                                   TypePayloadPart part,
                                   std::size_t& bitCount) noexcept {
    const TypePayload& payload =
        part == TypePayloadPart::baseline ? record.baseline : record.update;
    if (codec.write == nullptr || !valid_payload(codec, part, payload)) {
        return false;
    }
    encoding::bits::Writer writer = encoding::bits::Writer::measuring();
    std::size_t ignored = 0;
    if (!codec.write(codec.context, record.token, record.type, part, payload, writer)
        || !writer.finish(ignored) || writer.bit_count() > payload_limit(codec, part)) {
        return false;
    }
    bitCount = writer.bit_count();
    return true;
}

/** Runs one payload writer and requires the preflight length to remain exact. */
[[nodiscard]] bool write_payload(const TypePayloadCodec& codec,
                                 const EntityRecord& record,
                                 TypePayloadPart part,
                                 std::size_t expectedBits,
                                 encoding::bits::Writer& writer) noexcept {
    const TypePayload& payload =
        part == TypePayloadPart::baseline ? record.baseline : record.update;
    const std::size_t before = writer.bit_count();
    std::size_t ignored = 0;
    return codec.write != nullptr
           && codec.write(codec.context, record.token, record.type, part, payload, writer)
           && writer.finish(ignored) && writer.bit_count() >= before
           && writer.bit_count() - before == expectedBits;
}

/** Reads one payload through a bounded callback into temporary state. */
[[nodiscard]] bool read_payload(const TypePayloadCodec& codec,
                                const EntityToken& token,
                                EntityType type,
                                TypePayloadPart part,
                                encoding::bits::Reader& reader,
                                TypePayload& output) noexcept {
    const std::size_t limit = payload_limit(codec, part);
    if (codec.read == nullptr || limit > kMaximumTypePayloadBits) {
        return false;
    }
    const std::size_t before = reader.remaining_bits();
    TypePayload candidate{};
    std::uint64_t ignored = 0;
    if (!codec.read(codec.context, token, type, part, reader, candidate) || !reader.read(0, ignored)
        || candidate.byteCount > candidate.state.size()) {
        return false;
    }
    const std::size_t after = reader.remaining_bits();
    if (after > before || before - after > limit) {
        return false;
    }
    output = candidate;
    return true;
}

/** Resolves the type for an update-only record. */
[[nodiscard]] bool
resolve_type(const TypePayloadCodec& codec, const EntityToken& token, EntityType& output) noexcept {
    EntityType candidate = EntityType::count;
    if (codec.resolveType == nullptr || !codec.resolveType(codec.context, token, candidate)
        || !valid_type(candidate)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Builds a complete payload plan before the channel-2 header is touched. */
[[nodiscard]] bool
prepare_batch(const TypePayloadCodec& codec, const EntityBatch& batch, PayloadPlan& plan) noexcept {
    plan = {};
    if (!valid_raw_bubble(batch.defaultRawBubble)) {
        return false;
    }
    if (!batch.recordPresent) {
        return true;
    }
    if (!valid_record(batch.record)) {
        return false;
    }
    if ((batch.record.flags & entityCreate) != 0
        && !measure_payload(codec, batch.record, TypePayloadPart::baseline, plan.baselineBits)) {
        return false;
    }
    return (batch.record.flags & entityUpdate) == 0
           || measure_payload(codec, batch.record, TypePayloadPart::update, plan.updateBits);
}

/** Writes the five explicit flag bits or the update shortcut. */
[[nodiscard]] bool write_record_flags(encoding::bits::Writer& writer,
                                      std::uint16_t flags) noexcept {
    const bool shortcut = flags == entityUpdate;
    if (!write_flag(writer, shortcut)) {
        return false;
    }
    if (shortcut) {
        return true;
    }
    for (const std::uint16_t flag : kExplicitFlags) {
        if (!write_flag(writer, (flags & flag) != 0)) {
            return false;
        }
    }
    return true;
}

/** Reads the shortcut or five explicit flag bits. */
[[nodiscard]] bool read_record_flags(encoding::bits::Reader& reader,
                                     std::uint16_t& output) noexcept {
    bool shortcut = false;
    if (!read_flag(reader, shortcut)) {
        return false;
    }
    if (shortcut) {
        output = entityUpdate;
        return true;
    }
    std::uint16_t flags = 0;
    for (const std::uint16_t flag : kExplicitFlags) {
        bool present = false;
        if (!read_flag(reader, present)) {
            return false;
        }
        if (present) {
            flags |= flag;
        }
    }
    output = flags;
    return true;
}

/** Writes one inherited, byte-sized, or reset raw bubble. */
[[nodiscard]] bool write_record_bubble(encoding::bits::Writer& writer,
                                       std::uint16_t defaultRawBubble,
                                       std::uint16_t rawBubble) noexcept {
    const bool changed = rawBubble != defaultRawBubble;
    if (!write_flag(writer, changed)) {
        return false;
    }
    if (!changed) {
        return true;
    }
    const bool fits = rawBubble <= kMaximumRawBubble;
    return write_flag(writer, fits) && (!fits || writer.write(rawBubble, kRawBubbleWidth));
}

/** Reads one inherited, byte-sized, or reset raw bubble. */
[[nodiscard]] bool read_record_bubble(encoding::bits::Reader& reader,
                                      std::uint16_t defaultRawBubble,
                                      std::uint16_t& output) noexcept {
    bool changed = false;
    if (!read_flag(reader, changed)) {
        return false;
    }
    if (!changed) {
        output = defaultRawBubble;
        return true;
    }
    bool fits = false;
    if (!read_flag(reader, fits)) {
        return false;
    }
    if (!fits) {
        output = kNoRawBubble;
        return true;
    }
    std::uint64_t bubble = 0;
    if (!reader.read(kRawBubbleWidth, bubble)) {
        return false;
    }
    output = static_cast<std::uint16_t>(bubble);
    return true;
}

/** Writes one validated record body after its token and batch default. */
[[nodiscard]] bool write_record(encoding::bits::Writer& writer,
                                const TypePayloadCodec& codec,
                                const EntityRecord& record,
                                std::uint16_t defaultRawBubble,
                                const PayloadPlan& plan) noexcept {
    if (!write_record_flags(writer, record.flags)) {
        return false;
    }
    if ((record.flags & entityAnchor) != 0
        && (!write_flag(writer, record.anchorPresent)
            || (record.anchorPresent && !write_token(writer, record.anchor)))) {
        return false;
    }
    if (!write_record_bubble(writer, defaultRawBubble, record.rawBubble)) {
        return false;
    }
    if (record.flags == entityRemove
        && !writer.write(kStrictRemoveBodyBits, kSubrecordLengthWidth)) {
        return false;
    }
    if ((record.flags & entityCreate) != 0
        && (!writer.write(record.lifecycleRevision, kLifecycleRevisionWidth)
            || !writer.write(static_cast<std::uint8_t>(record.type), kEntityTypeWidth)
            || !write_payload(
                codec, record, TypePayloadPart::baseline, plan.baselineBits, writer))) {
        return false;
    }
    if ((record.flags & entityUpdate) != 0
        && !write_payload(codec, record, TypePayloadPart::update, plan.updateBits, writer)) {
        return false;
    }
    return (record.flags & entityRemove) == 0 || write_flag(writer, record.trailingState);
}

/** Reads one record body into temporary state. */
[[nodiscard]] bool read_record(encoding::bits::Reader& reader,
                               const TypePayloadCodec& codec,
                               std::uint16_t defaultRawBubble,
                               EntityRecord& output) noexcept {
    EntityRecord candidate{};
    candidate.token = output.token;
    if (!read_record_flags(reader, candidate.flags) || !valid_flags(candidate.flags)) {
        return false;
    }
    if ((candidate.flags & entityAnchor) != 0
        && (!read_flag(reader, candidate.anchorPresent)
            || (candidate.anchorPresent && !read_token(reader, candidate.anchor)))) {
        return false;
    }
    if (!read_record_bubble(reader, defaultRawBubble, candidate.rawBubble)) {
        return false;
    }
    if ((candidate.flags & (entityCreate | entityRemove)) == entityRemove) {
        std::uint64_t bitLength = 0;
        if (!reader.read(kSubrecordLengthWidth, bitLength) || bitLength != kStrictRemoveBodyBits) {
            return false;
        }
    }
    if ((candidate.flags & entityCreate) != 0) {
        std::uint64_t revision = 0;
        std::uint64_t type = 0;
        if (!reader.read(kLifecycleRevisionWidth, revision)
            || !reader.read(kEntityTypeWidth, type)) {
            return false;
        }
        candidate.lifecycleRevision = static_cast<std::uint8_t>(revision);
        candidate.type = static_cast<EntityType>(type);
        if (!valid_type(candidate.type)
            || !read_payload(codec,
                             candidate.token,
                             candidate.type,
                             TypePayloadPart::baseline,
                             reader,
                             candidate.baseline)) {
            return false;
        }
    } else if ((candidate.flags & entityUpdate) != 0
               && !resolve_type(codec, candidate.token, candidate.type)) {
        return false;
    }
    if ((candidate.flags & entityUpdate) != 0
        && !read_payload(codec,
                         candidate.token,
                         candidate.type,
                         TypePayloadPart::update,
                         reader,
                         candidate.update)) {
        return false;
    }
    if ((candidate.flags & entityRemove) != 0
        && (!read_flag(reader, candidate.trailingState) || candidate.trailingState)) {
        return false;
    }
    if (!valid_record(candidate)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Writes a prepared channel-2 batch without repeating callback preflight. */
[[nodiscard]] bool write_batch_fields(encoding::bits::Writer& writer,
                                      const TypePayloadCodec& codec,
                                      const EntityBatch& batch,
                                      const PayloadPlan& plan) noexcept {
    if (!write_flag(writer, batch.recordPresent)
        || (batch.recordPresent && !write_token(writer, batch.record.token))) {
        return false;
    }
    const bool defaultPresent = batch.defaultRawBubble != kNoRawBubble;
    if (!write_flag(writer, defaultPresent)
        || (defaultPresent && !writer.write(batch.defaultRawBubble, kRawBubbleWidth))) {
        return false;
    }
    return !batch.recordPresent
           || write_record(writer, codec, batch.record, batch.defaultRawBubble, plan);
}

/** Reads a channel-2 batch into temporary state. */
[[nodiscard]] bool read_batch_fields(encoding::bits::Reader& reader,
                                     const TypePayloadCodec& codec,
                                     EntityBatch& output) noexcept {
    EntityBatch candidate{};
    if (!read_flag(reader, candidate.recordPresent)
        || (candidate.recordPresent && !read_token(reader, candidate.record.token))) {
        return false;
    }
    bool defaultPresent = false;
    if (!read_flag(reader, defaultPresent)) {
        return false;
    }
    if (defaultPresent) {
        std::uint64_t bubble = 0;
        if (!reader.read(kRawBubbleWidth, bubble)) {
            return false;
        }
        candidate.defaultRawBubble = static_cast<std::uint16_t>(bubble);
    }
    if (candidate.recordPresent
        && !read_record(reader, codec, candidate.defaultRawBubble, candidate.record)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Writes common and all four fixed channels using one prepared entity plan. */
[[nodiscard]] bool write_frame_fields(encoding::bits::Writer& writer,
                                      const TypePayloadCodec& codec,
                                      const ExternalEntityFrame& frame,
                                      const PayloadPlan& plan) noexcept {
    if (!write_flag(writer, frame.commonPresent)
        || (frame.commonPresent && !write_common_state(writer, frame.common))) {
        return false;
    }
    // Channels 0 and 1 are empty until their type registries have server producers.
    if (!write_flag(writer, false) || !write_flag(writer, false)
        || !write_batch_fields(writer, codec, frame.entities, plan)) {
        return false;
    }
    // Channel 3 and its enclosing list stay absent in the generic fallback.
    return write_flag(writer, false) && write_flag(writer, false);
}

/** Reads common and all four fixed channels into temporary state. */
[[nodiscard]] bool read_frame_fields(encoding::bits::Reader& reader,
                                     const TypePayloadCodec& codec,
                                     ExternalEntityFrame& output) noexcept {
    ExternalEntityFrame candidate{};
    if (!read_flag(reader, candidate.commonPresent)
        || (candidate.commonPresent && !read_common_state(reader, candidate.common))) {
        return false;
    }
    bool present = false;
    if (!read_flag(reader, present) || present || !read_flag(reader, present) || present
        || !read_batch_fields(reader, codec, candidate.entities) || !read_flag(reader, present)
        || present || !read_flag(reader, present) || present) {
        return false;
    }
    output = candidate;
    return true;
}

} // namespace

/** Reads one channel-2 batch and commits the reader and output only on success. */
bool read_entity_batch(encoding::bits::Reader& reader,
                       const TypePayloadCodec& codec,
                       EntityBatch& output) noexcept {
    encoding::bits::Reader candidateReader = reader;
    EntityBatch candidate{};
    if (!read_batch_fields(candidateReader, codec, candidate)) {
        return false;
    }
    reader = candidateReader;
    output = candidate;
    return true;
}

/** Writes one channel-2 batch after a complete fail-closed preflight. */
bool write_entity_batch(encoding::bits::Writer& writer,
                        const TypePayloadCodec& codec,
                        const EntityBatch& batch) noexcept {
    PayloadPlan plan{};
    if (!prepare_batch(codec, batch, plan)) {
        return false;
    }
    encoding::bits::Writer measuring = encoding::bits::Writer::measuring();
    std::size_t ignored = 0;
    if (!write_batch_fields(measuring, codec, batch, plan) || !measuring.finish(ignored)) {
        return false;
    }
    return write_batch_fields(writer, codec, batch, plan);
}

/** Reads the fixed four-channel wrapper and commits no state on failure. */
bool read_external_entity_frame(encoding::bits::Reader& reader,
                                const TypePayloadCodec& codec,
                                ExternalEntityFrame& output) noexcept {
    encoding::bits::Reader candidateReader = reader;
    ExternalEntityFrame candidate{};
    if (!read_frame_fields(candidateReader, codec, candidate)) {
        return false;
    }
    reader = candidateReader;
    output = candidate;
    return true;
}

/** Writes the fixed four-channel wrapper after a complete fail-closed preflight. */
bool write_external_entity_frame(encoding::bits::Writer& writer,
                                 const TypePayloadCodec& codec,
                                 const ExternalEntityFrame& frame) noexcept {
    PayloadPlan plan{};
    if (!prepare_batch(codec, frame.entities, plan)) {
        return false;
    }
    encoding::bits::Writer measuring = encoding::bits::Writer::measuring();
    std::size_t ignored = 0;
    if (!write_frame_fields(measuring, codec, frame, plan) || !measuring.finish(ignored)) {
        return false;
    }
    return write_frame_fields(writer, codec, frame, plan);
}

} // namespace sunrise::middleware::gameplay::external
