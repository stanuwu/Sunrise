#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>

#include "composite_entity_codec_internal.h"

namespace sunrise::middleware::gameplay::external {
namespace {

namespace format = state::activity_sdk::format;
namespace wire = actor_wire;
namespace bits = middleware::encoding::bits;

/** The transform's packed angle code is seven bits. */
constexpr std::uint8_t kAngleWidth = 7;
/** Position components travel as raw 32-bit floats. */
constexpr std::uint8_t kFloatWidth = 32;
/** A transform names X, Y and Z. The fourth homogeneous lane is implied. */
constexpr std::size_t kPositionComponents = 3;

/** Channel-2 update contexts set native mode one before calling the SObject decoder. */
[[nodiscard]] format::RuntimeCodecFamily sobject_family() noexcept {
    return format::RuntimeCodecFamily::sobjectModeOne;
}

/** Raw float lanes must stay finite when retained for replay. */
[[nodiscard]] bool append_float(bits::Reader& reader, detail::MirrorBuilder& mirror) noexcept {
    std::uint64_t raw = 0;
    return detail::read_and_append(reader, mirror, kFloatWidth, raw)
           && std::isfinite(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
}

/** Native float-four encoding distinguishes points, directions, and explicit W. */
[[nodiscard]] bool append_position(bits::Reader& reader,
                                   detail::MirrorBuilder& mirror,
                                   const PositionProfile& profile) noexcept {
    bool point = false, direction = false;
    if (!detail::read_flag(reader, mirror, point)
        || (!point && !detail::read_flag(reader, mirror, direction))) {
        return false;
    }
    bool compressed = false;
    if (!direction && profile.selectorPresent) {
        if (!detail::read_flag(reader, mirror, compressed)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < kPositionComponents; ++index) {
        if (compressed) {
            if (!profile.hasWidths || profile.axisBits[index] >= kFloatWidth) {
                return false;
            }
            std::uint64_t raw = 0;
            if (!detail::read_and_append(reader, mirror, profile.axisBits[index], raw)) {
                return false;
            }
        } else if (!append_float(reader, mirror)) {
            return false;
        }
    }
    return point || direction || append_float(reader, mirror);
}

/** Placement baselines permit independent transform, parent, and stream-source deltas. */
[[nodiscard]] bool append_sobject_prefix(bits::Reader& reader,
                                         detail::MirrorBuilder& mirror,
                                         const CompositeEntityCodecContext& context) noexcept {
    bool transform = false;
    bool rotationShortcut = false;
    bool ignoredFlag = false;
    std::uint64_t ignored = 0;
    if (!detail::read_flag(reader, mirror, transform)) {
        return false;
    }
    if (transform) {
        // The general axis is a 19-bit native unit-vector code.
        constexpr std::uint8_t kAxisWidth = 19;
        if (!detail::read_flag(reader, mirror, rotationShortcut)
            || (rotationShortcut ? !detail::read_flag(reader, mirror, ignoredFlag)
                                 : !detail::read_and_append(reader, mirror, kAxisWidth, ignored))
            || !detail::read_and_append(reader, mirror, kAngleWidth, ignored)
            || !append_position(reader, mirror, context.positionProfile)) {
            return false;
        }
    }
    // The ordered roots follow the native SObject update reader.
    constexpr std::array<std::uint32_t, 2> kRelationSchemas{0x8080949BU, 0x8080949AU};
    detail::ResolverContext resolver{&context, sobject_family()};
    for (const auto schema : kRelationSchemas) {
        bool present = false;
        if (!detail::read_flag(reader, mirror, present)
            || (present && !detail::append_schema(reader, mirror, resolver, schema, nullptr))) {
            return false;
        }
    }
    return true;
}

/** The native compiled plan decides whether a component has any wire presence at all. */
static bool append_compiled_component(const CompositeEntityCodecContext& context,
                                      const SobjectDecodePlan& plan,
                                      bits::Reader& reader,
                                      detail::MirrorBuilder& mirror,
                                      std::uint32_t componentTag) noexcept {
    if (!plan.active) {
        return true;
    }
    bool present = false;
    if (!detail::read_flag(reader, mirror, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    // The component bitmap is bounded by the same work budget as its retained wire payload.
    std::array<std::uint8_t, kMaximumTypePayloadBits> bitmap{};
    if (plan.bitmapBits == 0 || plan.bitmapBits > bitmap.size()) {
        return false;
    }
    bitmap[0] = 1;
    detail::ResolverContext resolver{
        &context, sobject_family(), std::span(bitmap).first(plan.bitmapBits)};
    resolver.componentTag = componentTag;
    std::uint32_t poolOrdinal = 0;
    for (const auto& entry : plan.entries) {
        if (entry.guardBit >= plan.bitmapBits || entry.repeatCount > wire::kRuntimeValueCapacity) {
            return false;
        }
        const bool damagePool = componentTag == detail::kCombatantDamageComponentPlan
                                && entry.schemaHandle == detail::kCombatantDamagePoolSchema;
        const auto firstPool = poolOrdinal;
        if (damagePool) {
            if (poolOrdinal > (std::numeric_limits<std::uint32_t>::max)() - entry.repeatCount) {
                return false;
            }
            poolOrdinal += entry.repeatCount;
        }
        if (bitmap[entry.guardBit] == 0) {
            continue;
        }
        for (std::uint32_t index = 0; index < entry.repeatCount; ++index) {
            const auto base = static_cast<std::uint64_t>(entry.firstFieldBit)
                              + static_cast<std::uint64_t>(index) * entry.fieldBitStride;
            if (base > plan.bitmapBits) {
                return false;
            }
            resolver.firstFieldBit = static_cast<std::uint32_t>(base);
            resolver.damagePool = damagePool ? firstPool + index : format::kAbsentIndex;
            if (!detail::append_schema(reader, mirror, resolver, entry.schemaHandle, nullptr)) {
                return false;
            }
        }
    }
    return true;
}

/** Walks every ordered RSAT presence bit and each present component schema. */
[[nodiscard]] bool append_sobject_components(const CompositeEntityCodecContext& context,
                                             std::uint32_t rsatTag,
                                             bits::Reader& reader,
                                             detail::MirrorBuilder& mirror) noexcept {
    const format::SobjectRsat* rsat = detail::sobject_rsat(context, rsatTag);
    if (rsat == nullptr || rsat->flags != format::kSobjectRsatExact) {
        return false;
    }
    if (rsat->descriptors.first > context.sobjectDescriptors.size()
        || rsat->descriptors.count > context.sobjectDescriptors.size() - rsat->descriptors.first) {
        return false;
    }
    const auto descriptors =
        context.sobjectDescriptors.subspan(rsat->descriptors.first, rsat->descriptors.count);
    if (context.resolvePlan == nullptr) {
        return false;
    }
    for (const format::SobjectRsatDescriptor& descriptor : descriptors) {
        SobjectDecodePlan plan{};
        if (!context.resolvePlan(
                context.planContext, descriptor.componentTag, descriptor.schemaTag, plan)
            || !append_compiled_component(context, plan, reader, mirror, descriptor.componentTag)) {
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
            if (context.schemaFailure != nullptr) {
                wire::RuntimeDecodeResult failure{};
                failure.status = wire::CodecStatus::needsRuntimeSchema;
                context.schemaFailure(
                    descriptor.schemaTag, descriptor.componentTag, failure, nullptr);
            }
#endif
            return false;
        }
    }
    return true;
}
/** Resolves one update-only type-0 RSAT from committed session state. */
[[nodiscard]] bool registry_sobject_tag(const CompositeEntityCodecContext& context,
                                        const EntityToken& token,
                                        std::uint32_t& output) noexcept {
    if (context.registry == nullptr || !valid_token(token)) {
        return false;
    }
    const EntityBaselineSlot& slot = context.registry->slots[token.slot];
    if (!slot.occupied || slot.incarnation != token.incarnation || slot.type != EntityType::sobject
        || slot.rsatTag == 0) {
        return false;
    }
    output = slot.rsatTag;
    return true;
}

/** Dispatches one baseline or update through its SDK-selected grammar. */
[[nodiscard]] bool read_payload_impl(const CompositeEntityCodecContext& context,
                                     const EntityToken& token,
                                     EntityType type,
                                     TypePayloadPart part,
                                     const TypePayload* baseline,
                                     bits::Reader& reader,
                                     TypePayload& output) noexcept {
    const format::EntityTypeDefinition* definition = detail::entity_definition(context, type);
    if (definition == nullptr || (definition->flags & format::kEntityTypeStockEmittable) == 0) {
        return false;
    }
    detail::MirrorBuilder mirror{};
    std::uint32_t semanticTag = 0;
    if (part == TypePayloadPart::baseline) {
        if (baseline != nullptr || definition->baselineSchema == format::kAbsentIndex) {
            return false;
        }
        detail::ResolverContext resolver{&context, format::RuntimeCodecFamily::activity};
        if (!detail::append_schema(reader,
                                   mirror,
                                   resolver,
                                   definition->baselineSchema,
                                   type == EntityType::sobject ? &semanticTag : nullptr)) {
            return false;
        }
        if (type == EntityType::sobject) {
            bool placementIdentity = false;
            if (!detail::read_flag(reader, mirror, placementIdentity)) {
                return false;
            }
        }
        return mirror.finish(semanticTag, output);
    }
    if ((definition->flags & format::kEntityTypeUpdateSupported) == 0) {
        return false;
    }
    if ((definition->flags & format::kEntityTypeUpdateUsesSobjectRsat) != 0) {
        if (type != EntityType::sobject) {
            return false;
        }
        if (baseline != nullptr) {
            detail::MirrorView baselineView{};
            if (!detail::load_mirror(*baseline, baselineView)) {
                return false;
            }
            semanticTag = baselineView.header.semanticTag;
        } else if (!registry_sobject_tag(context, token, semanticTag)) {
            return false;
        }
        bool placementIdentity = false;
        if (baseline != nullptr) {
            detail::MirrorView view{};
            if (!detail::load_mirror(*baseline, view)
                || view.header.bitCount != kSobjectBaselineBits) {
                return false;
            }
            placementIdentity = (std::to_integer<unsigned>(view.bytes[4]) & 1U) != 0;
        } else {
            placementIdentity = context.registry->slots[token.slot].sobjectPlacement;
        }
        return semanticTag != 0
               && (!placementIdentity || append_sobject_prefix(reader, mirror, context))
               && append_sobject_components(context, semanticTag, reader, mirror)
               && mirror.finish(semanticTag, output);
    }
    if (definition->updateSchema == format::kAbsentIndex) {
        return false;
    }
    bool present = false;
    if (!detail::read_flag(reader, mirror, present)) {
        return false;
    }
    detail::ResolverContext resolver{&context, sobject_family()};
    return (!present
            || detail::append_schema(reader, mirror, resolver, definition->updateSchema, nullptr))
           && mirror.finish(0, output);
}

/** Replays exactly the meaningful bits from one validated mirror. */
[[nodiscard]] bool write_mirror(bits::Writer& writer, const detail::MirrorView& mirror) noexcept {
    bits::Reader reader(mirror.bytes);
    return bits::copy(reader, writer, mirror.header.bitCount);
}

} // namespace

namespace detail {

/** TypePayloadCodec reader adapter with no registry mutation. */
[[nodiscard]] bool read_payload(const void* raw,
                                const EntityToken& token,
                                EntityType type,
                                TypePayloadPart part,
                                const TypePayload* baseline,
                                bits::Reader& reader,
                                TypePayload& output) noexcept {
    const auto* const context = static_cast<const CompositeEntityCodecContext*>(raw);
    return context != nullptr && context->ready
           && read_payload_impl(*context, token, type, part, baseline, reader, output);
}

/** Revalidates a mirror through reflection before replaying it. */
[[nodiscard]] bool write_payload(const void* raw,
                                 const EntityToken& token,
                                 EntityType type,
                                 TypePayloadPart part,
                                 const TypePayload* baseline,
                                 const TypePayload& payload,
                                 bits::Writer& writer) noexcept {
    const auto* const context = static_cast<const CompositeEntityCodecContext*>(raw);
    MirrorView view{};
    if (context == nullptr || !context->ready || !load_mirror(payload, view)) {
        return false;
    }
    bits::Reader verifier(view.bytes);
    TypePayload canonical{};
    MirrorView canonicalView{};
    if (!read_payload_impl(*context, token, type, part, baseline, verifier, canonical)
        || !load_mirror(canonical, canonicalView) || canonical.byteCount != payload.byteCount
        || std::memcmp(canonical.state.data(), payload.state.data(), payload.byteCount) != 0
        || canonical.actorSource != payload.actorSource || canonical.damage != payload.damage) {
        return false;
    }
    return write_mirror(writer, view);
}

/** A cell profile is borrowed for one body and never changes another record's codec context. */
bool read_cell_payload(const void* raw,
                       const EntityToken& token,
                       EntityType type,
                       TypePayloadPart part,
                       const TypePayload* baseline,
                       std::uint16_t cell,
                       bits::Reader& reader,
                       TypePayload& output) noexcept {
    const auto* context = static_cast<const CompositeEntityCodecContext*>(raw);
    if (context == nullptr || !context->ready) {
        return false;
    }
    if (context->resolvePosition == nullptr) {
        return read_payload(context, token, type, part, baseline, reader, output);
    }
    CompositeEntityCodecContext selected = *context;
    if (!context->resolvePosition(
            context->positionContext, context->source, cell, selected.positionProfile)) {
        return false;
    }
    return read_payload(&selected, token, type, part, baseline, reader, output);
}

/** Replay selects the same package cell profile used to decode the retained body. */
bool write_cell_payload(const void* raw,
                        const EntityToken& token,
                        EntityType type,
                        TypePayloadPart part,
                        const TypePayload* baseline,
                        const TypePayload& payload,
                        std::uint16_t cell,
                        bits::Writer& writer) noexcept {
    const auto* context = static_cast<const CompositeEntityCodecContext*>(raw);
    if (context == nullptr || !context->ready) {
        return false;
    }
    if (context->resolvePosition == nullptr) {
        return write_payload(context, token, type, part, baseline, payload, writer);
    }
    CompositeEntityCodecContext selected = *context;
    if (!context->resolvePosition(
            context->positionContext, context->source, cell, selected.positionProfile)) {
        return false;
    }
    return write_payload(&selected, token, type, part, baseline, payload, writer);
}

/** Resolves an update-only entity type from its session registry. */
[[nodiscard]] bool
resolve_type(const void* raw, const EntityToken& token, EntityType& output) noexcept {
    const auto* const context = static_cast<const CompositeEntityCodecContext*>(raw);
    if (context == nullptr || context->registry == nullptr || !valid_token(token)) {
        return false;
    }
    const EntityBaselineSlot& slot = context->registry->slots[token.slot];
    if (!slot.occupied || slot.incarnation != token.incarnation
        || entity_definition(*context, slot.type) == nullptr) {
        return false;
    }
    output = slot.type;
    return true;
}

} // namespace detail

} // namespace sunrise::middleware::gameplay::external
