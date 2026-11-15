#include <map>
#include <optional>
#include <tuple>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../middleware/content/packages/reader/typed_array.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80808b.h"
#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../middleware/content/packages/tables/descriptor_embedded_placement_reader.h"
#include "../../../middleware/content/packages/tables/field_reader.h"
#include "activity_sdk_native_pack_internal.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {

namespace format = state::activity_sdk::format;
namespace tables = middleware::content::packages::tables;
namespace generated = middleware::content::packages::schema::generated;
namespace channel_auth = middleware::bap::activity_message::scriptable_auth;

/** Unsupported target shapes stay unavailable; malformed package edges stop generation. */
enum class ChannelCountStatus { exact, unsupported, ambiguous, malformed };

/** An exact count includes zero; a missing Variables component does not. */
struct ChannelCount final {
    ChannelCountStatus status{ChannelCountStatus::unsupported};
    std::uint32_t count{};
};

/** Cached package reads and indexed descriptor joins for output-channel extraction. */
struct ObjectChannelInventory final {
    const topology_inventory::Snapshot& topology;
    PackageContext& context;
    std::map<std::uint32_t, CachedTag> tags{};
    std::map<std::uint32_t, ChannelCount> classes{};
    std::map<std::uint32_t, ChannelCount> targets{};
    std::map<std::uint32_t, std::vector<std::uint32_t>> objectsByKey{};
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> slots{};
    std::vector<std::vector<const squads::DescriptorFact*>> descriptors{};

    /** Reads each referenced package at most once, preserving references across insertions. */
    [[nodiscard]] const CachedTag* package(std::uint32_t tag) {
        const auto found = tags.find(tag);
        if (found != tags.end()) {
            return &found->second;
        }
        CachedTag value{};
        if (tag == 0 || tag == format::kAbsentIndex
            || !read_tag(&context, tag, value.bytes, value.classId)) {
            return nullptr;
        }
        return &tags.emplace(tag, std::move(value)).first->second;
    }

    /** Reads the native eased-scalar array; its order is the type-24 output order. */
    [[nodiscard]] ChannelCount variables(const CachedTag& config,
                                         const tables::ComponentPair& pair) const noexcept {
        using Variables = generated::C80808bf1;
        using Row = generated::C80808be9;
        const std::size_t field = pair.primaryOffset + offsetof(Variables, h6a93495b);
        generated::TagArray<Row> array{};
        if (!tables::read(config.bytes, field, array) || array.count < 0) {
            return {ChannelCountStatus::malformed};
        }
        if (array.count == 0 && array.relative == 0) {
            return {ChannelCountStatus::exact};
        }
        std::size_t header{};
        reader::TypedArrayRows rows{};
        std::uint32_t padding{};
        if (!tables::relative_offset(
                field + offsetof(decltype(array), relative), array.relative, header)
            || !reader::read_typed_array(config.bytes,
                                         header,
                                         static_cast<std::uint64_t>(array.count),
                                         Row::kClass,
                                         sizeof(Row),
                                         rows)
            || !tables::read(
                config.bytes, header + reader::layout::TypedArrayHeader::kPadding, padding)
            || padding != 0) {
            return {ChannelCountStatus::malformed};
        }
        if (static_cast<std::uint64_t>(array.count) > channel_auth::kType24ChannelCapacity) {
            return {ChannelCountStatus::unsupported};
        }
        return {ChannelCountStatus::exact, static_cast<std::uint32_t>(array.count)};
    }

    /** Requires exactly one Variables component in the placed object's construction. */
    [[nodiscard]] ChannelCount class_count(std::uint32_t tag) {
        if (const auto found = classes.find(tag); found != classes.end()) {
            return found->second;
        }
        const CachedTag* root = package(tag);
        tables::PlacedClassDefinition definition{};
        if (root == nullptr || root->classId != tables::kPlacedClassDefinitionClass
            || !tables::placed_class_definition(root->bytes, definition)) {
            return {ChannelCountStatus::malformed};
        }
        ChannelCount result{};
        bool foundVariables = false;
        for (std::size_t index = 0; index < definition.builds.count; ++index) {
            tables::PlacedClassBuildRow build{};
            if (!tables::placed_class_build_at(root->bytes, definition.builds, index, build)) {
                return {ChannelCountStatus::malformed};
            }
            const CachedTag* config = package(build.configTag);
            tables::ComponentPair pair{};
            if (config == nullptr || config->classId != tables::kPlacedConfigClass
                || !tables::config_component_pair(config->bytes, build.configTag, pair)) {
                return {ChannelCountStatus::malformed};
            }
            if (pair.primaryClass != generated::C80808bf1::kClass) {
                continue;
            }
            if (foundVariables) {
                result = {ChannelCountStatus::ambiguous};
                break;
            }
            foundVariables = true;
            result = variables(*config, pair);
            if (result.status == ChannelCountStatus::malformed) {
                return result;
            }
        }
        classes.emplace(tag, result);
        return result;
    }

    /** Every authored placement and descriptor variant must agree on the receiving row count. */
    [[nodiscard]] ChannelCount target_count(std::uint32_t slotRow) {
        if (const auto found = targets.find(slotRow); found != targets.end()) {
            return found->second;
        }
        std::optional<std::uint32_t> count{};
        for (const squads::DescriptorFact* fact : descriptors[slotRow]) {
            if (!fact->complete) {
                return {ChannelCountStatus::unsupported};
            }
            const CachedTag* config = package(fact->configTag);
            tables::SlotDescriptor descriptor{fact->configTag,
                                              fact->componentClass,
                                              fact->senseSchema,
                                              fact->authSchema,
                                              fact->descriptorOffset};
            descriptor.slotType = static_cast<std::uint16_t>(topology.slots[slotRow].slotType);
            descriptor.slotIndex = static_cast<std::uint16_t>(topology.slots[slotRow].slotIndex);
            tables::DescriptorEmbeddedPlacementArray placements{};
            if (config == nullptr || config->classId != tables::kPlacedConfigClass
                || !tables::read(config->bytes,
                                 fact->descriptorOffset + tables::kDescriptorBubbleIndexOffset,
                                 descriptor.bubbleIndex)
                || !tables::descriptor_embedded_placements(config->bytes, descriptor, placements)) {
                return {ChannelCountStatus::malformed};
            }
            if (placements.count == 0) {
                return {ChannelCountStatus::unsupported};
            }
            for (std::uint64_t index = 0; index < placements.count; ++index) {
                tables::DescriptorEmbeddedPlacement placement{};
                if (!tables::descriptor_embedded_placement_at(
                        config->bytes, placements, index, placement)) {
                    return {ChannelCountStatus::malformed};
                }
                const ChannelCount value = class_count(placement.classListTag);
                if (value.status != ChannelCountStatus::exact) {
                    return value;
                }
                if (count && *count != value.count) {
                    return {ChannelCountStatus::ambiguous};
                }
                count = value.count;
            }
        }
        const ChannelCount result =
            count ? ChannelCount{ChannelCountStatus::exact, *count} : ChannelCount{};
        targets.emplace(slotRow, result);
        return result;
    }

    /** Resolves the descriptor's ClientRef without choosing among distinct object definitions. */
    [[nodiscard]] ChannelCount descriptor_count(const squads::DescriptorFact& fact) {
        const CachedTag* config = package(fact.configTag);
        generated::ClientRef reference{};
        if (config == nullptr || config->classId != tables::kPlacedConfigClass
            || !tables::read(config->bytes,
                             fact.descriptorOffset + offsetof(generated::C80804f3c, unk58),
                             reference)) {
            return {ChannelCountStatus::malformed};
        }
        if (reference.targetSlotType != tables::kDescriptorEmbeddedPlacementSlotType) {
            return {ChannelCountStatus::unsupported};
        }
        const auto found = objectsByKey.find(reference.targetKey);
        if (found == objectsByKey.end()) {
            return {ChannelCountStatus::unsupported};
        }
        std::uint32_t object = fact.objectIndex;
        if (topology.objects[object].objectKey != reference.targetKey) {
            if (found->second.size() != 1) {
                return {ChannelCountStatus::ambiguous};
            }
            object = found->second.front();
        }
        const auto target =
            slots.find({object, reference.targetSlotType, reference.targetSlotIndex});
        return target == slots.end() ? ChannelCount{} : target_count(target->second);
    }
};

/** Extracts the eased output count from each channel sensor's exact target object. */
bool attach_object_channel_counts(const topology_inventory::Snapshot& topology,
                                  const squads::Facts& facts,
                                  PackageContext& packageContext,
                                  topology_enrichment::Snapshot& enrichment) {
    if (topology.slots.size() != enrichment.slots.size()) {
        return false;
    }
    ObjectChannelInventory inventory{topology, packageContext};
    inventory.descriptors.resize(topology.slots.size());
    for (std::uint32_t row = 0; row < topology.objects.size(); ++row) {
        inventory.objectsByKey[topology.objects[row].objectKey].push_back(row);
    }
    for (std::uint32_t row = 0; row < topology.slots.size(); ++row) {
        const auto& slot = topology.slots[row];
        inventory.slots.emplace(std::tuple{slot.objectIndex, slot.slotType, slot.slotIndex}, row);
    }
    for (const auto& fact : facts.descriptors) {
        inventory.descriptors[fact.slotIndex].push_back(&fact);
    }
    for (std::uint32_t row = 0; row < topology.slots.size(); ++row) {
        const auto& slot = topology.slots[row];
        auto& enriched = enrichment.slots[row];
        if (slot.slotType != channel_auth::kType24SlotType
            || enriched.componentClass != channel_auth::kType24ComponentClass
            || enriched.senseSchema != channel_auth::kType24SenseSchema
            || enriched.authSchema != channel_auth::kType24Schema
            || (enriched.flags & format::kSlotSchemaJoinExact) == 0) {
            continue;
        }
        if (cancelled(packageContext.cancel, packageContext.cancelContext)) {
            return false;
        }
        std::optional<std::uint32_t> count{};
        ChannelCountStatus status = ChannelCountStatus::unsupported;
        for (const auto* fact : inventory.descriptors[row]) {
            const ChannelCount value =
                fact->complete ? inventory.descriptor_count(*fact) : ChannelCount{};
            status = value.status;
            if (status != ChannelCountStatus::exact) {
                break;
            }
            if (count && *count != value.count) {
                status = ChannelCountStatus::ambiguous;
                break;
            }
            count = value.count;
        }
        if (status == ChannelCountStatus::exact && count) {
            enriched.objectChannelCount = *count;
            enriched.flags |= format::kSlotObjectChannelsExact;
        } else {
            const char* reason = status == ChannelCountStatus::malformed   ? "malformed"
                                 : status == ChannelCountStatus::ambiguous ? "ambiguous"
                                                                           : "unsupported";
            core::log::writef(core::log::Channel::client,
                              core::log::Level::warn,
                              "ev=activity_sdk stage=object_channels slot=%u reason=%s",
                              row,
                              reason);
            if (status == ChannelCountStatus::malformed) {
                return false;
            }
        }
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
