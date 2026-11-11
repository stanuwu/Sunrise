#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "../../../middleware/content/packages/schema/generated/tag_classes_80808b.h"
#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../middleware/content/packages/tables/descriptor_embedded_placement_reader.h"
#include "../../../middleware/content/packages/tables/field_reader.h"
#include "activity_sdk_native_pack_internal.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {
namespace {

namespace tables = middleware::content::packages::tables;
namespace format = state::activity_sdk::format;
namespace generated = middleware::content::packages::schema::generated;

/** Selects the already indexed descriptor through the shared checked reader. */
struct DescriptorSearch final {
    std::uint32_t offset{};
    tables::SlotDescriptor value{};
    bool found{};
};

/** Retains the exact offset while allowing the descriptor reader to finish its scan. */
bool select_descriptor(void* context, const tables::SlotDescriptor& value) noexcept {
    auto& search = *static_cast<DescriptorSearch*>(context);
    if (value.descriptorOffset == search.offset) {
        search.value = value;
        search.found = true;
    }
    return true;
}

/** The hub targets an object through the descriptor's first payload field. */
struct HubDescriptor final {
    std::array<std::byte, 0x58> header{};
    std::uint32_t key{};
    std::uint16_t type{};
    std::uint16_t index{};
};

/** Counted package arrays point at a typed header, followed by their elements. */
struct ArrayField final {
    std::uint64_t count{};
    std::int64_t relative{};
};
struct ArrayHeader final {
    std::uint64_t count{};
    std::uint32_t type{};
    /** Reserved native array-header word; only zero is supported. */
    std::uint32_t padding{};
};
/** Both authored output-lane classes have the same element width. */
static_assert(sizeof(generated::C80808be9) == sizeof(generated::UnknownConstantVectorRow));

struct Package final {
    std::vector<std::byte> bytes{};
    std::uint32_t type{};
};

/** Reads a nonempty channel array only within the native four-lane capacity. */
[[nodiscard]] bool count(std::span<const std::byte> bytes,
                         std::size_t field,
                         std::uint32_t type,
                         std::uint32_t& output) noexcept {
    ArrayField array{};
    ArrayHeader header{};
    std::size_t target = 0;
    if (!tables::read(bytes, field, array) || array.count == 0
        || array.count > format::kOutputChannelCapacity
        || !tables::relative(bytes, field + offsetof(ArrayField, relative), target)
        || !tables::read(bytes, target, header) || header.count != array.count
        || header.type != type || header.padding != 0
        || array.count > (bytes.size() - target - sizeof header) / sizeof(generated::C80808be9)) {
        return false;
    }
    output = static_cast<std::uint32_t>(array.count);
    return true;
}

} // namespace

/** Resolves exact same-registry targets; unresolved channel bounds remain unavailable. */
bool attach_channel_bounds(const topology_inventory::Snapshot& topology,
                           const squads::Facts& facts,
                           PackageContext& packageContext,
                           topology_enrichment::Snapshot& enrichment) {
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    try {
        std::map<std::uint32_t, Package> packages{};
        auto read = [&](std::uint32_t tag) -> const Package* {
            auto found = packages.find(tag);
            if (found != packages.end()) {
                return &found->second;
            }
            Package value{};
            if (!read_tag(&packageContext, tag, value.bytes, value.type)) {
                return nullptr;
            }
            return &packages.emplace(tag, std::move(value)).first->second;
        };
        std::map<Key, std::uint32_t> slots{};
        std::map<std::uint32_t, std::vector<const squads::DescriptorFact*>> descriptors{};
        for (std::uint32_t index = 0; index < topology.slots.size(); ++index) {
            const auto& slot = topology.slots[index];
            const Key key{slot.objectIndex, slot.slotType, slot.slotIndex};
            auto [entry, inserted] = slots.emplace(key, index);
            if (!inserted) {
                entry->second = format::kAbsentIndex;
            }
        }
        for (const auto& descriptor : facts.descriptors) {
            descriptors[descriptor.slotIndex].push_back(&descriptor);
        }
        for (const auto& [slotRow, sources] : descriptors) {
            const auto& slot = topology.slots[slotRow];
            auto& enriched = enrichment.slots[slotRow];
            if (slot.slotType != format::kChannelSlotType
                || enriched.componentClass != format::kChannelComponentClass
                || enriched.senseSchema != format::kChannelSenseSchema
                || enriched.authSchema != format::kChannelAuthSchema
                || (enriched.flags & format::kSlotSchemaJoinExact) == 0) {
                continue;
            }
            std::uint32_t agreed = 0;
            bool exact = true;
            for (const auto* source : sources) {
                const Package* config = read(source->configTag);
                HubDescriptor hub{};
                if (config == nullptr || !source->complete
                    || !tables::read(std::span(config->bytes), source->descriptorOffset, hub)
                    || hub.key != topology.objects[slot.objectIndex].objectKey
                    || hub.type != format::kObjectSlotType) {
                    exact = false;
                    break;
                }
                const auto target = slots.find({slot.objectIndex, hub.type, hub.index});
                if (target == slots.end() || target->second == format::kAbsentIndex
                    || !descriptors.contains(target->second)) {
                    exact = false;
                    break;
                }
                for (const auto* object : descriptors.at(target->second)) {
                    const Package* placed = read(object->configTag);
                    DescriptorSearch search{object->descriptorOffset};
                    tables::DescriptorEmbeddedPlacementArray array{};
                    tables::DescriptorEmbeddedPlacement entry{};
                    if (placed == nullptr || !object->complete
                        || !tables::visit_slot_descriptors(
                            placed->bytes, object->configTag, hub.key, select_descriptor, &search)
                        || !search.found
                        || !tables::descriptor_embedded_placements(
                            placed->bytes, search.value, array)
                        || array.count != 1
                        || !tables::descriptor_embedded_placement_at(
                            placed->bytes, array, 0, entry)) {
                        exact = false;
                        break;
                    }
                    const Package* definition = read(entry.classListTag);
                    tables::PlacedClassDefinition builds{};
                    if (definition == nullptr
                        || definition->type != tables::kPlacedClassDefinitionClass
                        || !tables::placed_class_definition(definition->bytes, builds)) {
                        exact = false;
                        break;
                    }
                    std::uint32_t bound = 0;
                    for (std::uint64_t index = 0; index < builds.builds.count; ++index) {
                        tables::PlacedClassBuildRow build{};
                        if (!tables::placed_class_build_at(
                                definition->bytes, builds.builds, index, build)) {
                            exact = false;
                            break;
                        }
                        const Package* resource = read(build.configTag);
                        tables::ComponentPair pair{};
                        if (resource == nullptr) {
                            exact = false;
                            break;
                        }
                        if (!tables::config_component_pair(resource->bytes, build.configTag, pair)
                            || pair.primaryClass != generated::C80808bf1::kClass) {
                            continue;
                        }
                        std::uint32_t primary = 0, secondary = 0;
                        if (bound != 0
                            || pair.secondaryClass
                                   != generated::UnknownConstantPublisherResource::kClass
                            || !count(resource->bytes,
                                      pair.primaryOffset
                                          + offsetof(generated::C80808bf1, h6a93495b),
                                      generated::C80808be9::kClass,
                                      primary)
                            || !count(resource->bytes,
                                      pair.secondaryOffset
                                          + offsetof(generated::UnknownConstantPublisherResource,
                                                     vectorConstants),
                                      generated::UnknownConstantVectorRow::kClass,
                                      secondary)
                            || primary != secondary) {
                            exact = false;
                            break;
                        }
                        bound = primary;
                    }
                    if (!exact || bound == 0 || (agreed != 0 && agreed != bound)) {
                        exact = false;
                        break;
                    }
                    agreed = bound;
                }
                if (!exact) {
                    break;
                }
            }
            if (cancelled(packageContext.cancel, packageContext.cancelContext)) {
                return false;
            }
            if (exact && agreed != 0) {
                enriched.outputChannelCount = agreed;
                enriched.flags |= format::kSlotOutputChannelsExact;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
