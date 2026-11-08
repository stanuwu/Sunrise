#include "scriptable_catalog_names.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/tables/authored_placement_reader.h"
#include "../../../middleware/content/packages/tables/component_container_reader.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "../../../middleware/content/packages/tables/static_spatial_candidate_reader.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/scriptables/inline_name_evidence.h"
#include "../../../state/content/content_catalog.h"

namespace sunrise::client::content::activity::scriptables::internal {
namespace {

namespace catalog = state::build_data::scriptables;
namespace names_state = state::build_data::hash_names;
namespace tables = middleware::content::packages::tables;

struct HashDefinition final {
    std::uint32_t hash{};
    std::uint32_t definitionRow{};
};

[[nodiscard]] bool cancelled(NameCancelCheck check) noexcept {
    return check != nullptr && check();
}

/**
 * Finds one identifier name by its hash.
 * @param hashNames Rows sorted by hash, or null for the table State publishes.
 * @param hash Name hash.
 * @param name Receives the matching row.
 * @return True when the chosen table holds that exact hash.
 */
[[nodiscard]] bool find_identifier(const std::vector<names_state::Name>* hashNames,
                                   std::uint32_t hash,
                                   names_state::Name& name) noexcept {
    if (hashNames == nullptr) {
        return state::build_data::find_hash_name(hash, name);
    }
    const auto found =
        std::lower_bound(hashNames->begin(),
                         hashNames->end(),
                         hash,
                         [](const names_state::Name& row, std::uint32_t value) noexcept {
                             return row.hash < value;
                         });
    if (found == hashNames->end() || found->hash != hash) {
        return false;
    }
    name = *found;
    return true;
}

[[nodiscard]] bool same_candidate(const catalog::NameCandidate& candidate,
                                  std::string_view value,
                                  catalog::NameProvenance provenance,
                                  std::uint32_t sourceTag,
                                  std::uint32_t sourceClassId) noexcept {
    return candidate.provenance == provenance && candidate.length == value.size()
           && candidate.sourceTag == sourceTag && candidate.sourceClassId == sourceClassId
           && std::equal(value.begin(), value.end(), candidate.value.begin());
}

/** Appends one unique bounded name candidate at its stated evidence tier. */
template <typename NameRow>
[[nodiscard]] bool append_candidate(catalog::Snapshot& output,
                                    NameRow& name,
                                    std::string_view value,
                                    catalog::NameProvenance provenance,
                                    std::uint32_t sourceTag,
                                    std::uint32_t sourceClassId = 0) {
    if (value.empty() || value.size() >= catalog::kNameCapacity) {
        return true;
    }
    const std::size_t begin = name.firstCandidate;
    const std::size_t end = begin + name.candidateCount;
    for (std::size_t index = begin; index < end; ++index) {
        if (same_candidate(
                output.nameCandidates[index], value, provenance, sourceTag, sourceClassId)) {
            return true;
        }
    }
    if (output.nameCandidates.size() >= catalog::kNoRow || name.candidateCount == catalog::kNoRow) {
        return false;
    }
    catalog::NameCandidate candidate{};
    std::copy(value.begin(), value.end(), candidate.value.begin());
    candidate.length = static_cast<std::uint16_t>(value.size());
    candidate.provenance = provenance;
    candidate.sourceTag = sourceTag;
    candidate.sourceClassId = sourceClassId;
    output.nameCandidates.push_back(candidate);
    ++name.candidateCount;
    return true;
}

/** Selects one display value while retaining every package source that supplied it. */
[[nodiscard]] std::uint32_t uniform_value_candidate(const catalog::Snapshot& output,
                                                    std::uint32_t first,
                                                    std::uint32_t count) noexcept {
    if (count == 0 || first >= output.nameCandidates.size()) {
        return catalog::kNoRow;
    }
    const catalog::NameCandidate& expected = output.nameCandidates[first];
    for (std::uint32_t offset = 1; offset < count; ++offset) {
        const std::size_t row = static_cast<std::size_t>(first) + offset;
        if (row >= output.nameCandidates.size()) {
            return catalog::kNoRow;
        }
        const catalog::NameCandidate& candidate = output.nameCandidates[row];
        if (candidate.length != expected.length
            || !std::equal(expected.value.begin(),
                           expected.value.begin() + expected.length,
                           candidate.value.begin())) {
            return catalog::kNoRow;
        }
    }
    return first;
}

/** Selects a hash name only when the strongest available evidence tier is unique. */
[[nodiscard]] bool resolve_one_hash_name(catalog::Snapshot& output,
                                         std::span<const InlineName> inlineNames,
                                         std::span<const state::content::Definition> definitions,
                                         std::span<const HashDefinition> hashDefinitions,
                                         const std::vector<names_state::Name>* hashNames,
                                         NameCancelCheck cancel,
                                         std::uint32_t hash) {
    catalog::Name name{};
    name.hash = hash;
    name.firstCandidate = static_cast<std::uint32_t>(output.nameCandidates.size());
    const auto rawFirst =
        std::lower_bound(output.inlineNameCandidates.begin(),
                         output.inlineNameCandidates.end(),
                         hash,
                         [](const catalog::InlineNameCandidate& candidate,
                            std::uint32_t value) noexcept { return candidate.hash < value; });
    for (auto current = rawFirst;
         current != output.inlineNameCandidates.end() && current->hash == hash;
         ++current) {
        if (cancelled(cancel) || current->firstByte > output.inlineNameBytes.size()
            || current->byteCount > output.inlineNameBytes.size() - current->firstByte) {
            return false;
        }
        const char* value =
            reinterpret_cast<const char*>(output.inlineNameBytes.data() + current->firstByte);
        if (!append_candidate(output,
                              name,
                              std::string_view(value, current->byteCount),
                              catalog::NameProvenance::packageInline,
                              0)) {
            return false;
        }
    }
    for (const InlineName& candidate : inlineNames) {
        if (cancelled(cancel)) {
            return false;
        }
        if (candidate.hash == hash
            && !append_candidate(output,
                                 name,
                                 candidate.value,
                                 catalog::NameProvenance::packageInline,
                                 candidate.sourceTag)) {
            return false;
        }
    }
    const std::uint32_t inlineCount = name.candidateCount;
    const auto first =
        std::lower_bound(hashDefinitions.begin(),
                         hashDefinitions.end(),
                         hash,
                         [](const HashDefinition& definition, std::uint32_t value) noexcept {
                             return definition.hash < value;
                         });
    for (auto current = first; current != hashDefinitions.end() && current->hash == hash;
         ++current) {
        if (cancelled(cancel) || current->definitionRow >= definitions.size()) {
            return false;
        }
        const state::content::Definition& definition = definitions[current->definitionRow];
        if (!append_candidate(output,
                              name,
                              std::string_view(definition.name.data(), definition.nameLength),
                              catalog::NameProvenance::packagePath,
                              definition.tag,
                              definition.classId)) {
            return false;
        }
    }
    const std::uint32_t pathCount = name.candidateCount - inlineCount;
    if (cancelled(cancel)) {
        return false;
    }
    names_state::Name identifier{};
    if (find_identifier(hashNames, hash, identifier)
        && !append_candidate(output,
                             name,
                             std::string_view(identifier.name.data(), identifier.nameLength),
                             catalog::NameProvenance::packageIdentifierCandidate,
                             0)) {
        return false;
    }
    std::uint32_t bestFirst = name.firstCandidate;
    std::uint32_t bestCount = inlineCount;
    catalog::NameProvenance best = catalog::NameProvenance::packageInline;
    if (bestCount == 0 && pathCount != 0) {
        bestFirst += inlineCount;
        bestCount = pathCount;
        best = catalog::NameProvenance::packagePath;
    }
    if (bestCount == 0 && name.candidateCount != inlineCount + pathCount) {
        bestFirst += inlineCount + pathCount;
        bestCount = name.candidateCount - inlineCount - pathCount;
        best = catalog::NameProvenance::packageIdentifierCandidate;
    }
    name.selectedCandidate = uniform_value_candidate(output, bestFirst, bestCount);
    if (name.selectedCandidate != catalog::kNoRow) {
        name.provenance = best;
    }
    output.names.push_back(name);
    return true;
}

/** Selects an exact package-definition name only when the distinct path is unique. */
[[nodiscard]] bool resolve_one_tag_name(catalog::Snapshot& output,
                                        std::span<const state::content::Definition> definitions,
                                        NameCancelCheck cancel,
                                        std::uint32_t tag,
                                        std::uint32_t classId) {
    catalog::TagName name{};
    name.tag = tag;
    name.classId = classId;
    name.firstCandidate = static_cast<std::uint32_t>(output.nameCandidates.size());
    const auto first =
        std::lower_bound(definitions.begin(),
                         definitions.end(),
                         tag,
                         [](const state::content::Definition& definition,
                            std::uint32_t value) noexcept { return definition.tag < value; });
    for (auto current = first; current != definitions.end() && current->tag == tag; ++current) {
        if (cancelled(cancel)) {
            return false;
        }
        if (classId != 0 && current->classId != classId) {
            continue;
        }
        if (!append_candidate(output,
                              name,
                              std::string_view(current->name.data(), current->nameLength),
                              catalog::NameProvenance::packagePath,
                              current->tag,
                              current->classId)) {
            return false;
        }
    }
    name.selectedCandidate =
        uniform_value_candidate(output, name.firstCandidate, name.candidateCount);
    if (name.selectedCandidate != catalog::kNoRow) {
        name.provenance = catalog::NameProvenance::packagePath;
    }
    output.tagNames.push_back(name);
    return true;
}

/** @return The exact sorted hash-name row, or kNoRow when the hash is unresolved. */
[[nodiscard]] std::uint32_t hash_name_row(const catalog::Snapshot& output,
                                          std::uint32_t hash) noexcept {
    const auto found = std::lower_bound(
        output.names.begin(),
        output.names.end(),
        hash,
        [](const catalog::Name& row, std::uint32_t value) noexcept { return row.hash < value; });
    return found != output.names.end() && found->hash == hash
               ? static_cast<std::uint32_t>(found - output.names.begin())
               : catalog::kNoRow;
}

/** Finds one exact tag/class query row in the sorted package-definition name table. */
[[nodiscard]] std::uint32_t
tag_name_row(const catalog::Snapshot& output, std::uint32_t tag, std::uint32_t classId) noexcept {
    const std::pair<std::uint32_t, std::uint32_t> key{tag, classId};
    const auto found = std::lower_bound(
        output.tagNames.begin(),
        output.tagNames.end(),
        key,
        [](const catalog::TagName& row,
           const std::pair<std::uint32_t, std::uint32_t>& value) noexcept {
            return std::pair<std::uint32_t, std::uint32_t>{row.tag, row.classId} < value;
        });
    return found != output.tagNames.end() && found->tag == tag && found->classId == classId
               ? static_cast<std::uint32_t>(found - output.tagNames.begin())
               : catalog::kNoRow;
}

} // namespace

/** Resolves authored hashes and exact installed-package definition tags in one snapshot. */
bool resolve_names(catalog::Snapshot& output,
                   std::span<const InlineName> inlineNames,
                   const std::vector<state::content::Definition>* installed,
                   const std::vector<names_state::Name>* hashNames,
                   NameCancelCheck cancel) noexcept {
    try {
        std::vector<state::content::Definition> definitions{};
        if (installed != nullptr) {
            definitions = *installed;
        } else if (!state::content::snapshot(definitions)) {
            return false;
        }
        std::sort(definitions.begin(),
                  definitions.end(),
                  [](const state::content::Definition& first,
                     const state::content::Definition& second) noexcept {
                      if (first.tag != second.tag) {
                          return first.tag < second.tag;
                      }
                      if (first.classId != second.classId) {
                          return first.classId < second.classId;
                      }
                      return std::string_view(first.name.data(), first.nameLength)
                             < std::string_view(second.name.data(), second.nameLength);
                  });
        if (cancelled(cancel)) {
            return false;
        }
        std::vector<HashDefinition> hashDefinitions{};
        hashDefinitions.reserve(definitions.size());
        for (std::size_t row = 0; row < definitions.size(); ++row) {
            const state::content::Definition& definition = definitions[row];
            hashDefinitions.push_back({catalog::inline_name_evidence::hash(std::as_bytes(std::span(
                                           definition.name.data(), definition.nameLength))),
                                       static_cast<std::uint32_t>(row)});
        }
        std::sort(
            hashDefinitions.begin(),
            hashDefinitions.end(),
            [&definitions](const HashDefinition& first, const HashDefinition& second) noexcept {
                if (first.hash != second.hash) {
                    return first.hash < second.hash;
                }
                const state::content::Definition& firstDefinition =
                    definitions[first.definitionRow];
                const state::content::Definition& secondDefinition =
                    definitions[second.definitionRow];
                if (firstDefinition.tag != secondDefinition.tag) {
                    return firstDefinition.tag < secondDefinition.tag;
                }
                if (firstDefinition.classId != secondDefinition.classId) {
                    return firstDefinition.classId < secondDefinition.classId;
                }
                return std::string_view(firstDefinition.name.data(), firstDefinition.nameLength)
                       < std::string_view(secondDefinition.name.data(),
                                          secondDefinition.nameLength);
            });
        if (cancelled(cancel)) {
            return false;
        }
        std::vector<std::uint32_t> hashes{};
        hashes.reserve(output.bubbles.size() + output.states.size() + output.slots.size());
        for (const catalog::Bubble& bubble : output.bubbles) {
            hashes.push_back(bubble.nameHash);
        }
        for (const catalog::State& state : output.states) {
            hashes.push_back(state.stateHash);
        }
        for (const catalog::Slot& slot : output.slots) {
            hashes.push_back(slot.nameHash);
        }
        if (cancelled(cancel)) {
            return false;
        }
        std::sort(hashes.begin(), hashes.end());
        hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
        if (cancelled(cancel)) {
            return false;
        }
        output.names.reserve(hashes.size());
        for (const std::uint32_t hash : hashes) {
            if (cancelled(cancel)
                || !resolve_one_hash_name(
                    output, inlineNames, definitions, hashDefinitions, hashNames, cancel, hash)) {
                return false;
            }
        }
        for (catalog::Bubble& bubble : output.bubbles) {
            if (cancelled(cancel)) {
                return false;
            }
            bubble.nameRow = hash_name_row(output, bubble.nameHash);
        }
        for (catalog::State& state : output.states) {
            if (cancelled(cancel)) {
                return false;
            }
            state.nameRow = hash_name_row(output, state.stateHash);
        }
        for (catalog::Slot& slot : output.slots) {
            if (cancelled(cancel)) {
                return false;
            }
            slot.nameRow = hash_name_row(output, slot.nameHash);
        }

        std::vector<std::pair<std::uint32_t, std::uint32_t>> tags{};
        tags.reserve(
            output.states.size() * 2 + output.objects.size() * 2 + output.descriptors.size()
            + output.references.size() + output.authoredPlacements.size() * 2
            + output.containerPlacementLists.size() * 2 + output.containerPlacementOwners.size()
            + output.containerPlacements.size() + output.containerPlacementConfigs.size()
            + output.staticSpatialTables.size() * 2 + output.staticSpatialOwners.size() * 3
            + output.staticSpatialInstances.size() + output.triggerVolumeTables.size()
            + output.triggerVolumeInstances.size() * 2);
        const auto offer_tag = [&tags](std::uint32_t tag, std::uint32_t classId) {
            if (tag != 0) {
                tags.emplace_back(tag, classId);
            }
        };
        for (const catalog::State& state : output.states) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(state.entryTag, tables::kSliceEntryClass);
            offer_tag(state.registryTag, tables::kObjectRegistryClass);
        }
        for (const catalog::Object& object : output.objects) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(object.registryTag, tables::kObjectRegistryClass);
            offer_tag(object.objectTag, tables::kObjectClass);
        }
        for (const catalog::Descriptor& descriptor : output.descriptors) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(descriptor.configTag, tables::kPlacedObjectClass);
        }
        for (const catalog::TypedReference& reference : output.references) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(reference.sourceConfigTag, tables::kPlacedObjectClass);
        }
        for (const catalog::AuthoredPlacement& placement : output.authoredPlacements) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(placement.objectListTag, tables::kAuthoredPlacementListClass);
            offer_tag(placement.classListTag, 0);
        }
        for (const catalog::ContainerPlacementList& list : output.containerPlacementLists) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(list.objectListTag, tables::kAuthoredPlacementListClass);
            if (list.resourceResolved) {
                offer_tag(list.resourceTag, list.resourceClass);
            }
        }
        for (const catalog::ContainerPlacementOwner& owner : output.containerPlacementOwners) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(owner.containerTag, tables::kContainerClass);
        }
        for (const catalog::ContainerPlacement& placement : output.containerPlacements) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(placement.classListTag, tables::kPlacedClassDefinitionClass);
        }
        for (const catalog::ContainerPlacementConfig& config : output.containerPlacementConfigs) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(config.configTag, tables::kPlacedConfigClass);
        }
        for (const catalog::EmbeddedPlacement& placement : output.embeddedPlacements) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(placement.classListTag, tables::kPlacedClassDefinitionClass);
        }
        for (const catalog::StaticSpatialTable& table : output.staticSpatialTables) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(table.tableTag, tables::kStaticSpatialTableClass);
            offer_tag(table.boundsTag, tables::kStaticSpatialBoundsTableClass);
        }
        for (const catalog::StaticSpatialOwner& owner : output.staticSpatialOwners) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(owner.containerTag, tables::kContainerClass);
            offer_tag(owner.objectListTag, tables::kAuthoredPlacementListClass);
            offer_tag(owner.parentTag, tables::kStaticSpatialParentClass);
        }
        for (const catalog::StaticSpatialInstance& instance : output.staticSpatialInstances) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(instance.resourceTag, 0);
        }
        for (const catalog::TriggerVolumeTable& table : output.triggerVolumeTables) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(table.configTag, tables::kPlacedConfigClass);
        }
        for (const catalog::TriggerVolumeInstance& instance : output.triggerVolumeInstances) {
            if (cancelled(cancel)) {
                return false;
            }
            offer_tag(instance.classDefinitionTag, tables::kPlacedClassDefinitionClass);
            offer_tag(instance.shapeResourceTag, 0);
        }
        std::sort(tags.begin(), tags.end());
        tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
        if (cancelled(cancel)) {
            return false;
        }
        output.tagNames.reserve(tags.size());
        for (const auto& [tag, classId] : tags) {
            if (cancelled(cancel)
                || !resolve_one_tag_name(output, definitions, cancel, tag, classId)) {
                return false;
            }
        }
        for (catalog::State& state : output.states) {
            if (cancelled(cancel)) {
                return false;
            }
            state.entryNameRow = tag_name_row(output, state.entryTag, tables::kSliceEntryClass);
            state.registryNameRow =
                tag_name_row(output, state.registryTag, tables::kObjectRegistryClass);
        }
        for (catalog::Object& object : output.objects) {
            if (cancelled(cancel)) {
                return false;
            }
            object.registryNameRow =
                tag_name_row(output, object.registryTag, tables::kObjectRegistryClass);
            object.objectNameRow = tag_name_row(output, object.objectTag, tables::kObjectClass);
        }
        for (catalog::Descriptor& descriptor : output.descriptors) {
            if (cancelled(cancel)) {
                return false;
            }
            descriptor.configNameRow =
                tag_name_row(output, descriptor.configTag, tables::kPlacedObjectClass);
        }
        for (catalog::TypedReference& reference : output.references) {
            if (cancelled(cancel)) {
                return false;
            }
            reference.sourceConfigNameRow =
                tag_name_row(output, reference.sourceConfigTag, tables::kPlacedObjectClass);
        }
        for (catalog::AuthoredPlacement& placement : output.authoredPlacements) {
            if (cancelled(cancel)) {
                return false;
            }
            placement.objectListNameRow =
                tag_name_row(output, placement.objectListTag, tables::kAuthoredPlacementListClass);
            placement.classListNameRow = tag_name_row(output, placement.classListTag, 0);
        }
        for (catalog::ContainerPlacementList& list : output.containerPlacementLists) {
            if (cancelled(cancel)) {
                return false;
            }
            list.objectListNameRow =
                tag_name_row(output, list.objectListTag, tables::kAuthoredPlacementListClass);
            if (list.resourceResolved) {
                list.resourceNameRow = tag_name_row(output, list.resourceTag, list.resourceClass);
            }
        }
        for (catalog::ContainerPlacementOwner& owner : output.containerPlacementOwners) {
            if (cancelled(cancel)) {
                return false;
            }
            owner.containerNameRow =
                tag_name_row(output, owner.containerTag, tables::kContainerClass);
        }
        for (catalog::ContainerPlacement& placement : output.containerPlacements) {
            if (cancelled(cancel)) {
                return false;
            }
            placement.classListNameRow =
                tag_name_row(output, placement.classListTag, tables::kPlacedClassDefinitionClass);
        }
        for (catalog::ContainerPlacementConfig& config : output.containerPlacementConfigs) {
            if (cancelled(cancel)) {
                return false;
            }
            config.configNameRow =
                tag_name_row(output, config.configTag, tables::kPlacedConfigClass);
        }
        for (catalog::EmbeddedPlacement& placement : output.embeddedPlacements) {
            if (cancelled(cancel)) {
                return false;
            }
            placement.classListNameRow =
                tag_name_row(output, placement.classListTag, tables::kPlacedClassDefinitionClass);
        }
        for (catalog::StaticSpatialTable& table : output.staticSpatialTables) {
            if (cancelled(cancel)) {
                return false;
            }
            table.tableNameRow =
                tag_name_row(output, table.tableTag, tables::kStaticSpatialTableClass);
            table.boundsNameRow =
                tag_name_row(output, table.boundsTag, tables::kStaticSpatialBoundsTableClass);
        }
        for (catalog::StaticSpatialOwner& owner : output.staticSpatialOwners) {
            if (cancelled(cancel)) {
                return false;
            }
            owner.containerNameRow =
                tag_name_row(output, owner.containerTag, tables::kContainerClass);
            owner.objectListNameRow =
                tag_name_row(output, owner.objectListTag, tables::kAuthoredPlacementListClass);
            owner.parentNameRow =
                tag_name_row(output, owner.parentTag, tables::kStaticSpatialParentClass);
        }
        for (catalog::StaticSpatialInstance& instance : output.staticSpatialInstances) {
            if (cancelled(cancel)) {
                return false;
            }
            instance.resourceNameRow = tag_name_row(output, instance.resourceTag, 0);
        }
        for (catalog::TriggerVolumeTable& table : output.triggerVolumeTables) {
            if (cancelled(cancel)) {
                return false;
            }
            table.configNameRow = tag_name_row(output, table.configTag, tables::kPlacedConfigClass);
        }
        for (catalog::TriggerVolumeInstance& instance : output.triggerVolumeInstances) {
            if (cancelled(cancel)) {
                return false;
            }
            instance.classDefinitionNameRow = tag_name_row(
                output, instance.classDefinitionTag, tables::kPlacedClassDefinitionClass);
            instance.shapeResourceNameRow = tag_name_row(output, instance.shapeResourceTag, 0);
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::scriptables::internal
