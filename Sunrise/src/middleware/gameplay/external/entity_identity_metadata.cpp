#include "entity_identity_metadata.h"

#include <array>
#include <limits>

#include "../../content/packages/schema/generated/tag_classes_80806a.h"
#include "../../content/packages/schema/generated/tag_classes_80809c.h"
#include "../../content/packages/schema/generated/tag_classes_80809f.h"

namespace sunrise::middleware::gameplay::external {
namespace {
namespace wire = middleware::bap::activity_message::wire_schema;
namespace format = state::activity_sdk::format;
namespace generated = middleware::content::packages::schema::generated;
using Metadata = state::gameplay::entity_identity::Metadata;
/** Baseline roots: a squad is one bare ClientRef; the player root has a raw 8-byte child. */
constexpr std::uint32_t kClientRefSchema = generated::ClientRef::kClass;
constexpr std::uint32_t kPlayerSchema = generated::C80806abd::kClass;
constexpr std::uint32_t kPlayerBytesSchema = generated::C80809f7b::kClass;
/** The squad baseline's decoded struct offsets and wire widths. */
constexpr std::array<std::uint32_t, 3> kSquadOffsets{0, 4, 6};
constexpr std::array<std::uint8_t, 3> kSquadWidths{32, 7, 16};
/** A player baseline is eight unsigned wire bytes, with no numeric identity assumption. */
constexpr std::size_t kPlayerBytes = 8;
constexpr std::uint8_t kByteWidth = 8;

/**
 * Only one exact native baseline definition selects metadata semantics.
 * @param catalog Pinned SDK rows.
 * @param type Wire entity type.
 * @param expected Verified schema handle.
 * @return True for one exact matching definition.
 */
bool has_schema(const state::activity_sdk::Snapshot& catalog,
                EntityType type,
                std::uint32_t expected) noexcept {
    bool found = false;
    for (const auto& row : catalog->entity_type_definitions()) {
        if (row.entityType != static_cast<std::uint32_t>(type)) {
            continue;
        }
        if (found || row.baselineSchema != expected
            || (row.flags & format::kEntityTypeDefinitionExact) == 0) {
            return false;
        }
        found = true;
    }
    return found;
}

/**
 * A squad identity needs all three exact fields and no sentinel values.
 * @param catalog Pinned field rows.
 * @param values Complete decoded baseline scalars.
 * @param output Receives the complete ClientRef.
 * @return True when the reference is usable.
 */
bool squad_metadata(const state::activity_sdk::Snapshot& catalog,
                    std::span<const wire::RuntimeDecodedValue> values,
                    Metadata& output) noexcept {
    if (values.size() != kSquadOffsets.size()) {
        return false;
    }
    std::array<std::int64_t, kSquadOffsets.size()> scalars{};
    std::array<bool, kSquadOffsets.size()> seen{};
    for (const auto& value : values) {
        if (value.schemaHandle != kClientRefSchema || value.role != wire::ValueRole::scalar
            || !value.present || value.occurrence != 0
            || value.fieldRow >= catalog->runtime_fields().size()) {
            return false;
        }
        const auto offset = catalog->runtime_fields()[value.fieldRow].structOffset;
        bool matched = false;
        for (std::size_t index = 0; index < kSquadOffsets.size(); ++index) {
            if (offset != kSquadOffsets[index]) {
                continue;
            }
            if (seen[index] || value.width != kSquadWidths[index]) {
                return false;
            }
            if (index == 0) {
                if (value.kind != wire::ValueKind::unsignedInteger || value.unsignedValue == 0
                    || value.unsignedValue >= format::kAbsentIndex) {
                    return false;
                }
                scalars[index] = static_cast<std::int64_t>(value.unsignedValue);
            } else {
                if (value.kind != wire::ValueKind::signedInteger || value.signedValue < 0
                    || value.signedValue > (index == 1 ? 126 : 32767)) {
                    return false;
                }
                scalars[index] = value.signedValue;
            }
            seen[index] = true;
            matched = true;
        }
        if (!matched) {
            return false;
        }
    }
    output.squad = {static_cast<std::uint32_t>(scalars[0]),
                    static_cast<std::uint16_t>(scalars[2]),
                    static_cast<std::uint8_t>(scalars[1])};
    output.hasSquad = true;
    return true;
}

/**
 * Raw player bytes keep their reflected order without an endian conversion.
 * @param values Complete decoded baseline scalars.
 * @param output Receives all eight bytes.
 * @return True when each byte occurrence appears exactly once.
 */
bool player_metadata(std::span<const wire::RuntimeDecodedValue> values, Metadata& output) noexcept {
    if (values.size() != kPlayerBytes) {
        return false;
    }
    std::array<bool, kPlayerBytes> seen{};
    for (const auto& value : values) {
        if (value.schemaHandle != kPlayerBytesSchema || value.role != wire::ValueRole::scalar
            || !value.present || value.kind != wire::ValueKind::unsignedInteger
            || value.width != kByteWidth || value.unsignedValue > 0xFFU
            || value.occurrence >= seen.size() || seen[value.occurrence]) {
            return false;
        }
        seen[value.occurrence] = true;
        output.playerBroadcast[value.occurrence] = static_cast<std::byte>(value.unsignedValue);
    }
    output.hasPlayerBroadcast = true;
    return true;
}
} // namespace

/** Ambiguous or incomplete class joins leave the object type unknown. */
bool extract_sobject_object_type(std::span<const state::activity_sdk::format::ActorClass> classes,
                                 std::uint32_t rsatTag,
                                 std::uint8_t& output) noexcept {
    output = 0;
    if (rsatTag == 0 || rsatTag == 0xFFFFFFFFU) {
        return false;
    }
    const state::activity_sdk::format::ActorClass* selected = nullptr;
    for (const auto& row : classes) {
        if (row.rsatTag != rsatTag) {
            continue;
        }
        if (selected || row.definitionTag == 0 || row.definitionTag == 0xFFFFFFFFU
            || row.rsatReverseDefinitionTag != row.definitionTag || row.objectType > 0xFFU) {
            return false;
        }
        selected = &row;
    }
    if (!selected) {
        return false;
    }
    output = static_cast<std::uint8_t>(selected->objectType);
    return true;
}

/**
 * Unknown metadata stays absent while transport retains the generic entity identity.
 * @param catalog Pinned SDK catalog used to decode the baseline.
 * @param record Accepted create record.
 * @param output Receives known metadata, cleared on failure.
 * @param status Optional exact extraction result.
 * @return True when the supported metadata was extracted, including an empty test identity.
 */
bool extract_entity_identity_metadata(const state::activity_sdk::Snapshot& catalog,
                                      const EntityRecord& record,
                                      Metadata& output,
                                      MetadataStatus* status) noexcept {
    output = {};
    const auto finish = [status](MetadataStatus value) noexcept {
        if (status != nullptr) {
            *status = value;
        }
        return value == MetadataStatus::complete;
    };
    if ((record.flags & entityCreate) == 0) {
        return finish(MetadataStatus::notCreate);
    }
    Metadata candidate{};
    if (record.type == EntityType::sobject) {
        if (!composite_sobject_rsat(record.baseline, candidate.rsatTag)) {
            return finish(MetadataStatus::malformedPayload);
        }
        candidate.hasRsat = true;
        if (catalog) {
            candidate.hasObjectType = extract_sobject_object_type(
                catalog->actor_classes(), candidate.rsatTag, candidate.objectType);
        }

    } else if (record.type == EntityType::test) {
        return finish(MetadataStatus::complete);
    } else {
        if (catalog == nullptr) {
            return finish(MetadataStatus::missingCatalog);
        }
        const auto schema = record.type == EntityType::squad ? kClientRefSchema : kPlayerSchema;
        if ((record.type != EntityType::squad && record.type != EntityType::playerBroadcast)
            || !has_schema(catalog, record.type, schema)) {
            return finish(MetadataStatus::unsupportedSchema);
        }
        std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> values{};
        wire::RuntimeDecodeResult result{};
        if (!decode_composite_entity_payload(
                catalog, record.type, TypePayloadPart::baseline, record.baseline, values, result)) {
            return finish(MetadataStatus::malformedPayload);
        }
        const auto decoded = std::span(values).first(result.valueCount);
        if (!(record.type == EntityType::squad ? squad_metadata(catalog, decoded, candidate)
                                               : player_metadata(decoded, candidate))) {
            return finish(MetadataStatus::invalidReference);
        }
    }
    output = candidate;
    return finish(MetadataStatus::complete);
}
/**
 * Unobserved source components preserve the retained relation.
 * @param record Accepted record with a staged type payload.
 * @param output Receives the known relation or clear; cleared on failure.
 * @return True only for a valid staged source relation in an SObject update.
 */
bool extract_actor_source_reference(
    const EntityRecord& record,
    state::gameplay::entity_identity::ActorSourceReference& output) noexcept {
    output = {};
    if (record.type != EntityType::sobject || !(record.flags & entityUpdate)
        || (record.flags & entityRemove)) {
        return false;
    }
    const auto& source = record.update.actorSource;
    if (!source.known) {
        return false;
    }
    if (source.present
        && (source.key == 0 || source.key == 0xFFFFFFFFU || source.type > 126
            || source.index > 32767)) {
        return false;
    }
    if (source.present) {
        output = source;
    }
    output.known = true;
    return true;
}
} // namespace sunrise::middleware::gameplay::external
