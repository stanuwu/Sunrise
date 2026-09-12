#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

#include "composite_entity_codec_internal.h"

namespace sunrise::middleware::gameplay::external {
namespace {

namespace format = state::activity_sdk::format;
namespace wire = actor_wire;
namespace bits = middleware::encoding::bits;

/** The walker view stores the type code and codec parameter 2 as one byte each. */
constexpr std::uint32_t kMaximumByteField = 0xFFU;
/** A schema reference the mirror retains must fit one 32-bit tag. */
constexpr std::uint64_t kMaximumSemanticTag = 0xFFFFFFFFULL;

/** Resolves one schema only when it supports the selected codec family. */
[[nodiscard]] bool
find_schema(const void* raw, std::uint32_t handle, wire::runtime::SchemaView& output) noexcept {
    const auto* const context = static_cast<const detail::ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    const format::RuntimeSchema* row = detail::runtime_schema(*context->catalog, handle);
    if (row == nullptr && context->catalog->resolveAdditionalSchema != nullptr) {
        return context->catalog->resolveAdditionalSchema(
                   context->catalog->planContext, handle, output)
               && output.handle == handle && output.row == handle;
    }
    // A schema carrying any other flag is not one this codec can walk.
    constexpr std::uint32_t kAllowedFlags =
        format::kRuntimeSchemaExact | format::kRuntimeSchemaArrayRegion;
    const bool arrayRegion =
        row != nullptr && (row->flags & format::kRuntimeSchemaArrayRegion) != 0;
    if (row == nullptr || (row->flags & format::kRuntimeSchemaExact) == 0
        || (row->flags & ~kAllowedFlags) != 0 || arrayRegion != (row->arrayElementCount != 0)
        || (context->family == format::RuntimeCodecFamily::activity
            && (row->codecFamilies & static_cast<std::uint32_t>(context->family)) == 0)) {
        return false;
    }
    const auto schemas = context->catalog->runtimeSchemas;
    const auto fields = context->catalog->runtimeFields;
    if (row < schemas.data() || row >= schemas.data() + schemas.size()
        || row->fields.first > fields.size()
        || row->fields.count > fields.size() - row->fields.first) {
        return false;
    }
    output.row = static_cast<std::uint32_t>(row - schemas.data());
    output.handle = row->handle;
    output.arrayLength = row->arrayElementCount;
    output.firstField = row->fields.first;
    output.fieldCount = row->fields.count;
    output.structSize = row->decodedSize;
    if (context->family != format::RuntimeCodecFamily::activity
        && context->catalog->resolveSchemaLayout != nullptr
        && !context->catalog->resolveSchemaLayout(
            context->catalog->planContext, row->handle, output.structSize)) {
        return false;
    }
    return true;
}

/** Resolves one schema by its stable generated row index. */
[[nodiscard]] bool
read_schema(const void* raw, std::uint32_t rowIndex, wire::runtime::SchemaView& output) noexcept {
    const auto* const context = static_cast<const detail::ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    if (rowIndex >= context->catalog->runtimeSchemas.size()) {
        return find_schema(raw, rowIndex, output) && output.row == rowIndex;
    }
    return find_schema(raw, context->catalog->runtimeSchemas[rowIndex].handle, output)
           && output.row == rowIndex;
}

/** Converts one SDK field row into the generic runtime walker view. */
[[nodiscard]] bool
read_field(const void* raw, std::uint32_t rowIndex, wire::runtime::FieldView& output) noexcept {
    const auto* const context = static_cast<const detail::ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    if (rowIndex >= context->catalog->runtimeFields.size()) {
        std::uint32_t nested = format::kAbsentIndex;
        if (context->catalog->resolveAdditionalField == nullptr
            || !context->catalog->resolveAdditionalField(
                context->catalog->planContext, rowIndex, output, nested)
            || output.row != rowIndex) {
            return false;
        }
        if (nested != format::kAbsentIndex) {
            wire::runtime::SchemaView target{};
            if (!find_schema(raw, nested, target)) {
                return false;
            }
            output.nestedSchemaRow = target.row;
        }
        if ((context->family != format::RuntimeCodecFamily::sobjectModeZero
             && output.typeCode == 30)
            || (context->family == format::RuntimeCodecFamily::sobjectModeOne
                && output.typeCode == 20)) {
            output.typeCode = 18;
        }
        if (context->family == format::RuntimeCodecFamily::sobjectModeOne
            && output.typeCode == 29) {
            output.typeCode = 43;
        }
        return true;
    }
    const format::RuntimeField& row = context->catalog->runtimeFields[rowIndex];
    if (row.schemaIndex >= context->catalog->runtimeSchemas.size()
        || (row.flags & format::kRuntimeFieldExact) == 0 || row.typeCode > kMaximumByteField) {
        return false;
    }
    std::uint32_t nestedRow = wire::runtime::kAbsentRuntimeRow;
    if (row.nestedHandle != format::kAbsentIndex) {
        wire::runtime::SchemaView nested{};
        if (!find_schema(raw, row.nestedHandle, nested)) {
            return false;
        }
        nestedRow = nested.row;
    }
    std::int32_t bias = 0;
    if (row.bias != format::kAbsentSignedValue) {
        if (row.bias < (std::numeric_limits<std::int32_t>::min)()
            || row.bias > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        bias = static_cast<std::int32_t>(row.bias);
    }
    const bool dynamicArray = (row.flags & format::kRuntimeFieldDynamicArray) != 0;
    const bool quantized =
        row.typeCode == static_cast<std::uint32_t>(format::RuntimeFieldType::quantizedFloat)
        || row.typeCode == 42;
    output = {};
    output.row = rowIndex;
    output.nestedSchemaRow = nestedRow;
    output.structOffset = context->family == format::RuntimeCodecFamily::activity
                              ? row.structOffset
                              : row.alternateOffset;
    if (context->family != format::RuntimeCodecFamily::activity
        && context->catalog->resolveFieldLayout != nullptr) {
        output.hasBitmapOffset = context->catalog->resolveFieldLayout(
            context->catalog->planContext,
            context->catalog->runtimeSchemas[row.schemaIndex].handle,
            row.ordinal,
            output.bitmapOffset);
    }
    output.biasOrDynamic = quantized      ? static_cast<std::int32_t>(row.codecParameters[0])
                           : dynamicArray ? 1
                                          : bias;
    output.widthOrCountOffset = quantized || dynamicArray || row.typeCode == 44
                                    ? static_cast<std::int32_t>(row.codecParameters[1])
                                : row.bits == format::kAbsentIndex
                                    ? 0
                                    : static_cast<std::int32_t>(row.bits);
    output.typeCode = static_cast<std::uint8_t>(row.typeCode);
    if ((context->family != format::RuntimeCodecFamily::sobjectModeZero && output.typeCode == 30)
        || (context->family == format::RuntimeCodecFamily::sobjectModeOne
            && output.typeCode == 20)) {
        output.typeCode = 18;
    }
    if (context->family == format::RuntimeCodecFamily::sobjectModeOne && output.typeCode == 29) {
        output.typeCode = 43;
    }
    output.presence =
        static_cast<std::uint8_t>((row.flags & format::kRuntimeFieldPresenceBit) != 0);
    output.parameter2 = static_cast<std::uint8_t>(
        row.codecParameters[2] <= kMaximumByteField ? row.codecParameters[2] : 0);
    return true;
}

/** Accepts a union selector only when its family-specific SDK row exists. */
[[nodiscard]] bool validate_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const context = static_cast<const detail::ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    const std::uint32_t family = static_cast<std::uint32_t>(context->family);
    return std::any_of(context->catalog->runtimeTypes.begin(),
                       context->catalog->runtimeTypes.end(),
                       [family, typeCode](const format::RuntimeTypeDefinition& row) {
                           return row.typeCode == typeCode && (row.codecFamilies & family) != 0;
                       });
}

/** Accepts one exact family-specific unsupported type only when it consumes no bits. */
[[nodiscard]] bool zero_bit_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const context = static_cast<const detail::ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr) {
        return false;
    }
    const std::uint32_t family = static_cast<std::uint32_t>(context->family);
    const format::RuntimeTypeDefinition* match = nullptr;
    for (const format::RuntimeTypeDefinition& row : context->catalog->runtimeTypes) {
        if (row.typeCode != typeCode || (row.codecFamilies & family) == 0) {
            continue;
        }
        if (match != nullptr) {
            return false;
        }
        match = &row;
    }
    return match != nullptr && (match->flags & format::kRuntimeTypeDefinitionExact) != 0
           && (match->flags & format::kRuntimeTypeUnsupported) != 0 && match->fixedBits == 0
           && match->minimumBits == 0 && match->maximumBits == 0 && match->reserved == 0;
}

/** Per-field presence updates the component's compiled selection bitmap. */
static bool record_presence(const void* raw, std::uint32_t bit, bool present) noexcept {
    const auto& context = *static_cast<const detail::ResolverContext*>(raw);
    if (bit >= context.presence.size()) {
        return false;
    }
    context.presence[bit] = present ? 1 : 0;
    return true;
}

/** T11 reference aliases share wire forms without changing other codec families. */
static std::uint8_t canonical_type(const void* raw, std::uint8_t type) noexcept {
    const auto& context = *static_cast<const detail::ResolverContext*>(raw);
    // Mode zero uses a different raw reference body.
    if (context.family == format::RuntimeCodecFamily::sobjectModeZero && type == 27) {
        return 0;
    }
    if (context.family != format::RuntimeCodecFamily::sobjectModeZero && type == 30) {
        return 18;
    }
    if (context.family == format::RuntimeCodecFamily::sobjectModeOne) {
        if (type == 20) {
            return 18;
        }
        if (type == 29) {
            return 43;
        }
    }
    return type;
}

/** The native action selector chooses one exact SDK-declared payload schema. */
static bool
resolve_command_payload(const void* raw, std::uint8_t selector, std::uint32_t& output) noexcept {
    output = 0;
    const auto& context = *static_cast<const detail::ResolverContext*>(raw);
    if (context.catalog->catalog == nullptr) {
        return false;
    }
    bool found = false;
    for (const auto& command : context.catalog->catalog->actor_command_definitions()) {
        if (command.selector != selector) {
            continue;
        }
        if (found || command.flags != format::kActorCommandDefinitionExact
            || command.payloadHandle == 0 || command.payloadHandle == format::kAbsentIndex) {
            return false;
        }
        found = true;
        output = command.payloadHandle;
    }
    return found;
}

/** Builds borrowed reflection callbacks over one stable resolver context. */
[[nodiscard]] wire::RuntimeSchemaResolver make_resolver(detail::ResolverContext& context) noexcept {
    wire::RuntimeSchemaResolver resolver{};
    resolver.context = &context;
    resolver.findSchema = find_schema;
    resolver.readSchema = read_schema;
    resolver.readField = read_field;
    resolver.validateType = validate_type;
    resolver.isZeroBitType = zero_bit_type;
    resolver.positionProfile = &context.catalog->positionProfile;
    resolver.recordPresence = context.presence.empty() ? nullptr : record_presence;
    resolver.firstFieldBit = context.firstFieldBit;
    resolver.canonicalType = canonical_type;
    resolver.nativeCommandEmptyShortcut =
        context.family == format::RuntimeCodecFamily::sobjectModeOne;
    resolver.resolveCommandPayload = resolve_command_payload;
    return resolver;
}

/** Decodes one retained typed mirror through its published executable schema. */
bool decode_composite_entity_payload_impl(const state::activity_sdk::Snapshot& catalog,
                                          EntityType type,
                                          TypePayloadPart part,
                                          const TypePayload& payload,
                                          std::span<wire::RuntimeDecodedValue> values,
                                          wire::RuntimeDecodeResult& result) noexcept {
    result = {};
    if (catalog == nullptr) {
        return false;
    }
    CompositeEntityCodecContext context{};
    context.catalog = catalog;
    context.entityTypes = catalog->entity_type_definitions();
    context.sobjectRsats = catalog->sobject_rsats();
    context.sobjectDescriptors = catalog->sobject_rsat_descriptors();
    context.rsatSchemas = catalog->rsat_schemas();
    context.rsatFields = catalog->rsat_fields();
    context.sobjectBindings = catalog->sobject_rsat_field_bindings();
    context.runtimeSchemas = catalog->runtime_schemas();
    context.runtimeFields = catalog->runtime_fields();
    context.runtimeTypes = catalog->runtime_type_definitions();
    context.ready = true;
    const format::EntityTypeDefinition* definition = detail::entity_definition(context, type);
    const std::uint32_t schemaHandle = definition == nullptr ? format::kAbsentIndex
                                       : part == TypePayloadPart::baseline
                                           ? definition->baselineSchema
                                           : definition->updateSchema;
    detail::MirrorView mirror{};
    if (schemaHandle == format::kAbsentIndex || !detail::load_mirror(payload, mirror)) {
        return false;
    }
    detail::ResolverContext resolverContext{&context,
                                            part == TypePayloadPart::baseline
                                                ? format::RuntimeCodecFamily::activity
                                                : format::RuntimeCodecFamily::sobjectModeOne};
    const wire::RuntimeSchemaResolver resolver = make_resolver(resolverContext);
    if (part == TypePayloadPart::update) {
        bits::Reader reader(mirror.bytes);
        std::uint64_t present = 0;
        if (mirror.header.bitCount == 0 || !reader.read(1, present)) {
            return false;
        }
        if (!present) {
            result.status = wire::CodecStatus::complete;
            result.bitsConsumed = 1;
            return mirror.header.bitCount == 1;
        }
        const bool decoded =
            wire::decode_full_schema_prefix(schemaHandle, reader, resolver, values, result);
        ++result.bitsConsumed;
        return decoded && result.status == wire::CodecStatus::complete && !result.valuesTruncated
               && result.bitsConsumed == mirror.header.bitCount;
    }
    return wire::decode_full_schema(
               schemaHandle, mirror.bytes, mirror.header.bitCount, resolver, values, result)
           && (result.status == wire::CodecStatus::complete
               || result.status == wire::CodecStatus::completeWithPadding)
           && !result.valuesTruncated;
}

/** Validates the complete row closure required by the composite codec. */
[[nodiscard]] bool valid_catalog_rows(const CompositeEntityCodecContext& context) noexcept {
    if (context.entityTypes.size() != static_cast<std::size_t>(EntityType::count)
        || context.sobjectRsats.empty() || context.sobjectDescriptors.empty()
        || context.sobjectBindings.size() != context.rsatFields.size()
        || context.runtimeSchemas.empty() || context.runtimeFields.empty()
        || context.runtimeTypes.empty()
        || (context.positionCompression != SobjectPositionCompression::disabled
            && context.positionCompression != SobjectPositionCompression::enabledRaw)) {
        return false;
    }
    std::array<bool, static_cast<std::size_t>(EntityType::count)> found{};
    for (const format::EntityTypeDefinition& row : context.entityTypes) {
        if (row.entityType >= found.size() || found[row.entityType]
            || (row.flags & format::kEntityTypeDefinitionExact) == 0) {
            return false;
        }
        const bool stock = (row.flags & format::kEntityTypeStockEmittable) != 0;
        const bool update = (row.flags & format::kEntityTypeUpdateSupported) != 0;
        const bool dynamic = (row.flags & format::kEntityTypeUpdateUsesSobjectRsat) != 0;
        if ((stock && row.baselineSchema == format::kAbsentIndex)
            || (dynamic && (!update || row.updateSchema != format::kAbsentIndex))
            || (!dynamic && update && row.updateSchema == format::kAbsentIndex)) {
            return false;
        }
        if (stock && detail::runtime_schema(context, row.baselineSchema) == nullptr) {
            return false;
        }
        if (!dynamic && update && detail::runtime_schema(context, row.updateSchema) == nullptr) {
            return false;
        }
        found[row.entityType] = true;
    }
    return std::all_of(found.begin(), found.end(), [](bool value) { return value; });
}

} // namespace

namespace detail {

/** Finds one unique runtime schema handle in the pinned view. */
[[nodiscard]] const format::RuntimeSchema*
runtime_schema(const CompositeEntityCodecContext& context, std::uint32_t handle) noexcept {
    for (const format::RuntimeSchema& row : context.runtimeSchemas) {
        if (row.handle == handle) {
            return &row;
        }
    }
    return nullptr;
}

/** Finds one installed RSAT in its generated tag order. */
[[nodiscard]] const format::SobjectRsat* sobject_rsat(const CompositeEntityCodecContext& context,
                                                      std::uint32_t tag) noexcept {
    const auto found =
        std::lower_bound(context.sobjectRsats.begin(),
                         context.sobjectRsats.end(),
                         tag,
                         [](const auto& row, auto value) { return row.rsatTag < value; });
    return found != context.sobjectRsats.end() && found->rsatTag == tag ? &*found : nullptr;
}

/** Checks the complete 17-bit entity-token domain. */
[[nodiscard]] bool valid_token(const EntityToken& token) noexcept {
    return token.slot <= kMaximumEntitySlot && token.incarnation <= kMaximumEntityIncarnation;
}

/** Validates and opens one callback-owned canonical mirror. */
[[nodiscard]] bool load_mirror(const TypePayload& payload, MirrorView& output) noexcept {
    if (payload.byteCount < sizeof(MirrorHeader) || payload.byteCount > payload.state.size()) {
        return false;
    }
    MirrorHeader header{};
    std::memcpy(&header, payload.state.data(), sizeof(header));
    const std::size_t byteCount = (static_cast<std::size_t>(header.bitCount) + 7U) / 8U;
    if (header.identity != kMirrorIdentity || header.reserved != 0
        || header.bitCount > kMaximumTypePayloadBits
        || payload.byteCount != sizeof(header) + byteCount) {
        return false;
    }
    output.header = header;
    output.bytes = {payload.state.data() + sizeof(header), byteCount};
    return true;
}

/** Reads one field and retains the same wire bits. */
[[nodiscard]] bool read_and_append(bits::Reader& reader,
                                   MirrorBuilder& mirror,
                                   std::uint8_t width,
                                   std::uint64_t& output) noexcept {
    return reader.read(width, output) && mirror.append(output, width);
}

/** Reads and retains one required boolean field. */
[[nodiscard]] bool read_flag(bits::Reader& reader, MirrorBuilder& mirror, bool& output) noexcept {
    std::uint64_t value = 0;
    if (!read_and_append(reader, mirror, kFlagWidth, value)) {
        return false;
    }
    output = value != 0;
    return true;
}

/** Decodes, re-encodes, and retains one complete reflected schema body. */
[[nodiscard]] bool append_schema(bits::Reader& reader,
                                 MirrorBuilder& mirror,
                                 ResolverContext& resolverContext,
                                 std::uint32_t schemaHandle,
                                 std::uint32_t* semanticTag) noexcept {
    const wire::RuntimeSchemaResolver resolver = make_resolver(resolverContext);
    std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> values{};
    wire::RuntimeDecodeResult result{};
    if (!wire::decode_full_schema_prefix(schemaHandle, reader, resolver, values, result)
        || result.status != wire::CodecStatus::complete || result.valuesTruncated
        || result.valueCount > values.size()) {
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
        if (resolverContext.catalog->schemaValues != nullptr && result.valueCount <= values.size())
            resolverContext.catalog->schemaValues(schemaHandle,
                                                  std::span(values).first(result.valueCount));
        if (resolverContext.catalog->schemaFailure != nullptr) {
            const auto field = result.valueCount != 0 && result.valueCount <= values.size()
                                   ? values[result.valueCount - 1].fieldRow
                                   : format::kAbsentIndex;
            resolverContext.catalog->schemaFailure(schemaHandle,
                                                   field,
                                                   result,
                                                   result.valueCount != 0
                                                           && result.valueCount <= values.size()
                                                       ? &values[result.valueCount - 1]
                                                       : nullptr);
        }
#endif
        return false;
    }
    std::array<wire::RuntimeDraftValue, wire::kRuntimeValueCapacity> draft{};
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
    if (resolverContext.catalog->schemaValues != nullptr)
        resolverContext.catalog->schemaValues(schemaHandle,
                                              std::span(values).first(result.valueCount));
#endif
    for (std::size_t index = 0; index < result.valueCount; ++index) {
        static_cast<wire::RuntimeDraftValue&>(draft[index]) = values[index];
    }
    std::array<std::byte, kMaximumTypePayloadBits / 8U> encoded{};
    std::size_t written = 0;
    std::size_t writtenBits = 0;
    wire::CodecStatus status = wire::CodecStatus::malformed;
    if (!wire::encode_full_schema(
            schemaHandle,
            std::span<const wire::RuntimeDraftValue>{draft.data(), result.valueCount},
            resolver,
            encoded,
            written,
            writtenBits,
            status)
        || status != wire::CodecStatus::complete || writtenBits != result.bitsConsumed
        || !mirror.append(std::span<const std::byte>{encoded.data(), written}, writtenBits)) {
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
        if (resolverContext.catalog->schemaFailure != nullptr) {
            result.status = status;
            resolverContext.catalog->schemaFailure(
                schemaHandle, static_cast<std::uint32_t>(writtenBits), result, nullptr);
        }
#endif
        return false;
    }
    if (semanticTag != nullptr) {
        const std::uint64_t nullableType =
            static_cast<std::uint32_t>(format::RuntimeFieldType::nullableTag);
        bool selected = false;
        bool found = false;
        std::uint32_t tag = 0;
        for (std::size_t index = 0; index < result.valueCount; ++index) {
            const wire::RuntimeDecodedValue& value = values[index];
            if (value.role == wire::ValueRole::variantSelector
                && value.unsignedValue == nullableType) {
                selected = true;
            } else if (selected && value.role == wire::ValueRole::schemaReference && value.present
                       && value.unsignedValue <= kMaximumSemanticTag) {
                if (found) {
                    return false;
                }
                found = true;
                tag = static_cast<std::uint32_t>(value.unsignedValue);
            }
        }
        if (!found || tag == 0 || tag == format::kAbsentIndex) {
            return false;
        }
        *semanticTag = tag;
    }
    if (resolverContext.componentTag == 0x80C70EDCU && schemaHandle == 0x80C70EDCU) {
        wire::runtime::SchemaView component{}, source{}, reference{};
        wire::runtime::FieldView componentField{}, sourceField{};
        if (!find_schema(&resolverContext, schemaHandle, component) || component.fieldCount != 1
            || !read_field(&resolverContext, component.firstField, componentField)
            || componentField.typeCode != 1 || componentField.structOffset != 0
            || !read_schema(&resolverContext, componentField.nestedSchemaRow, source)
            || source.handle != 0x808090E6U || source.fieldCount == 0
            || !read_field(&resolverContext, source.firstField, sourceField)
            || sourceField.typeCode != 1 || sourceField.structOffset != 0
            || !read_schema(&resolverContext, sourceField.nestedSchemaRow, reference)
            || reference.handle != 0x80809C42U || reference.fieldCount != 3) {
            return false;
        }
        std::array<const wire::RuntimeDecodedValue*, 3> fields{};
        for (const auto& value : std::span(values).first(result.valueCount)) {
            if (value.schemaHandle != reference.handle || value.role != wire::ValueRole::scalar) {
                continue;
            }
            if (value.fieldRow < reference.firstField || value.fieldRow >= reference.firstField + 3
                || value.occurrence != 0 || !value.present) {
                return false;
            }
            auto& field = fields[value.fieldRow - reference.firstField];
            if (field != nullptr) {
                return false;
            }
            field = &value;
        }
        if (fields[0] != nullptr && fields[1] != nullptr && fields[2] != nullptr) {
            const auto key = fields[0]->unsignedValue;
            const auto type = fields[1]->signedValue;
            const auto index = fields[2]->signedValue;
            if (key == 0x811C9DC5U && type == -1 && index == -1) {
                mirror.actorSource = {};
                mirror.actorSource.known = true;
            } else if (key != 0 && key != 0xFFFFFFFFU && key <= 0xFFFFFFFFU && type >= 0
                       && type <= 126 && index >= 0 && index <= 32767) {
                mirror.actorSource.key = static_cast<std::uint32_t>(key);
                mirror.actorSource.type = static_cast<std::uint8_t>(type);
                mirror.actorSource.index = static_cast<std::uint16_t>(index);
                mirror.actorSource.known = true;
                mirror.actorSource.present = true;
            }
        }
    }
    return true;
}

/** Resolves one exact channel-2 type contract from the SDK. */
[[nodiscard]] const format::EntityTypeDefinition*
entity_definition(const CompositeEntityCodecContext& context, EntityType type) noexcept {
    for (const format::EntityTypeDefinition& row : context.entityTypes) {
        if (row.entityType == static_cast<std::uint32_t>(type)) {
            return (row.flags & format::kEntityTypeDefinitionExact) != 0 ? &row : nullptr;
        }
    }
    return nullptr;
}

/** Pins one authenticated SDK snapshot and resets stale registry ownership. */
[[nodiscard]] bool bind_context(CompositeEntityCodecContext& context,
                                EntityBaselineRegistry& registry,
                                const state::activity_sdk::Snapshot& catalog,
                                SobjectPositionCompression positionCompression) noexcept {
    if (catalog == nullptr
        || (positionCompression != SobjectPositionCompression::disabled
            && positionCompression != SobjectPositionCompression::enabledRaw)) {
        return false;
    }
    if (registry.catalog != catalog) {
        registry.catalog = catalog;
        registry.slots.fill({});
    }
    context.catalog = catalog;
    context.entityTypes = catalog->entity_type_definitions();
    context.sobjectRsats = catalog->sobject_rsats();
    context.sobjectDescriptors = catalog->sobject_rsat_descriptors();
    context.rsatSchemas = catalog->rsat_schemas();
    context.rsatFields = catalog->rsat_fields();
    context.sobjectBindings = catalog->sobject_rsat_field_bindings();
    context.runtimeSchemas = catalog->runtime_schemas();
    context.runtimeFields = catalog->runtime_fields();
    context.runtimeTypes = catalog->runtime_type_definitions();
    context.registry = &registry;
    context.positionCompression = positionCompression;
    context.ready = valid_catalog_rows(context);
    return context.ready;
}

} // namespace detail

/** Decodes one retained typed mirror through its published executable schema. */
bool decode_composite_entity_payload(const state::activity_sdk::Snapshot& catalog,
                                     EntityType type,
                                     TypePayloadPart part,
                                     const TypePayload& payload,
                                     std::span<wire::RuntimeDecodedValue> values,
                                     wire::RuntimeDecodeResult& result) noexcept {
    return decode_composite_entity_payload_impl(catalog, type, part, payload, values, result);
}

/**
 * Reads only the private create-mirror layout, never a legacy typed payload.
 * @param payload Composite SObject baseline retained by the wire decoder.
 * @param output Receives its RSAT tag, or zero on failure.
 * @return True when the complete create mirror carries a valid tag.
 */
bool composite_sobject_rsat(const TypePayload& payload, std::uint32_t& output) noexcept {
    output = 0;
    detail::MirrorView view{};
    if (!detail::load_mirror(payload, view) || view.header.bitCount != kSobjectBaselineBits
        || view.header.semanticTag == 0 || view.header.semanticTag == format::kAbsentIndex) {
        return false;
    }
    output = view.header.semanticTag;
    return true;
}

/** Binds the currently published authenticated SDK. */
bool initialize_composite_entity_codec(CompositeEntityCodecContext& context,
                                       EntityBaselineRegistry& registry,
                                       SobjectPositionCompression positionCompression) noexcept {
    state::activity_sdk::Snapshot catalog = state::activity_sdk::snapshot();
    return detail::bind_context(context, registry, catalog, positionCompression);
}

/** Binds one caller-pinned authenticated SDK. */
bool initialize_composite_entity_codec_from_catalog(
    CompositeEntityCodecContext& context,
    EntityBaselineRegistry& registry,
    const state::activity_sdk::Snapshot& catalog,
    SobjectPositionCompression positionCompression) noexcept {
    return detail::bind_context(context, registry, catalog, positionCompression);
}

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Binds deterministic row spans in focused test binaries only. */
bool initialize_composite_entity_codec_for_test(
    CompositeEntityCodecContext& context,
    EntityBaselineRegistry& registry,
    const CompositeEntityCatalogFixture& fixture,
    SobjectPositionCompression positionCompression) noexcept {
    CompositeEntityCodecContext candidate{};
    candidate.entityTypes = fixture.entityTypes;
    candidate.sobjectRsats = fixture.sobjectRsats;
    candidate.sobjectDescriptors = fixture.sobjectDescriptors;
    candidate.rsatSchemas = fixture.rsatSchemas;
    candidate.rsatFields = fixture.rsatFields;
    candidate.sobjectBindings = fixture.sobjectBindings;
    candidate.runtimeSchemas = fixture.runtimeSchemas;
    candidate.runtimeFields = fixture.runtimeFields;
    candidate.runtimeTypes = fixture.runtimeTypes;
    candidate.resolvePlan = fixture.resolvePlan;
    candidate.planContext = fixture.planContext;
    candidate.registry = &registry;
    candidate.positionCompression = positionCompression;
    candidate.ready = valid_catalog_rows(candidate);
    if (!candidate.ready) {
        return false;
    }
    std::destroy_at(&registry);
    std::construct_at(&registry);
    context = candidate;
    return true;
}
#endif

} // namespace sunrise::middleware::gameplay::external
