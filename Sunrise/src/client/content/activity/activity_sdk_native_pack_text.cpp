#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/tables/activity_display_name_reader.h"
#include "activity_sdk_dialogue_group_index.h"
#include "activity_sdk_native_pack_internal.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {
namespace {

namespace format = state::activity_sdk::format;
namespace dialogue_groups = dialogue_group_index;
namespace display = middleware::content::packages::tables::activity_display_names;

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> bytes, std::size_t offset, Value& output) noexcept {
    output = {};
    if (offset > bytes.size() || sizeof output > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&output, bytes.data() + offset, sizeof output);
    return true;
}

/** Applies one signed blob-relative offset. @return False when the result leaves the blob. */
[[nodiscard]] bool
add_relative(std::size_t member, std::int64_t relative, std::size_t& target) noexcept {
    if (relative >= 0) {
        const auto distance = static_cast<std::uint64_t>(relative);
        if (distance > (std::numeric_limits<std::size_t>::max)() - member) {
            return false;
        }
        target = member + static_cast<std::size_t>(distance);
        return true;
    }
    const auto distance = static_cast<std::uint64_t>(-(relative + 1)) + 1U;
    if (distance > member) {
        return false;
    }
    target = member - static_cast<std::size_t>(distance);
    return true;
}

/** Reads one array field's data offset and count, checking its declared class and stride. */
[[nodiscard]] bool read_array(std::span<const std::byte> bytes,
                              std::size_t field,
                              std::size_t stride,
                              std::uint32_t expectedClass,
                              std::size_t& data,
                              std::size_t& count) noexcept {
    std::uint64_t rawCount = 0;
    std::int64_t relative = 0;
    std::size_t header = 0;
    std::uint64_t repeated = 0;
    std::uint32_t classId = 0;
    std::uint32_t padding = 0;
    if (!read_value(bytes, field, rawCount) || rawCount > format::kAbsentIndex
        || !read_value(bytes, field + 8U, relative)) {
        return false;
    }
    if (rawCount == 0 && relative == 0) {
        data = 0;
        count = 0;
        return true;
    }
    if (!add_relative(field + 8U, relative, header) || !read_value(bytes, header, repeated)
        || repeated != rawCount || !read_value(bytes, header + 8U, classId)
        || classId != expectedClass || !read_value(bytes, header + 12U, padding) || padding != 0
        || rawCount > (std::numeric_limits<std::size_t>::max)() / stride) {
        return false;
    }
    data = header + 16U;
    count = static_cast<std::size_t>(rawCount);
    return data <= bytes.size() && count * stride <= bytes.size() - data;
}

[[nodiscard]] bool copy_text(std::string_view value, authored_scene::Text& output) noexcept {
    output = {};
    if (value.empty() || value.size() >= output.value.size()
        || value.size() > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    std::copy(value.begin(), value.end(), output.value.begin());
    output.length = static_cast<std::uint16_t>(value.size());
    return true;
}

struct AuthoredTextCandidate final {
    display::Reference reference{};
    std::uint32_t slotIndex{};
    std::uint32_t cueIndex{format::kAbsentIndex};
    std::uint32_t definitionHash{};
};

/** The two localized fields are one directive element, not two selectable directives. */
struct AuthoredDirectiveCandidate final {
    display::Reference title{};
    display::Reference description{};
    std::uint32_t slotIndex{};
    std::uint32_t nameHash{};
    std::int32_t elementIndex{-1};
    std::uint32_t elementCount{};
};

} // namespace

/**
 * Keeps authored group ordinals and task counts tied to the exact objective slot.
 * @param topology Canonical slots.
 * @param facts Descriptor sources for those slots.
 * @param packageContext Installed package reader.
 * @param output Receives the native group rows.
 * @return False when repeated definitions disagree or the package reader fails.
 */
bool attach_combat_objective_groups(const topology_inventory::Snapshot& topology,
                                    const squads::Facts& facts,
                                    PackageContext& packageContext,
                                    authored_scene::Snapshot& output) {
    // Objective descriptors contain 40-byte groups at +136 and 40-byte tasks at group +16.
    constexpr std::size_t kGroupsField = 136U;
    constexpr std::size_t kTasksField = 16U;
    constexpr std::size_t kRowStride = 40U;
    constexpr std::uint32_t kGroupClass = 0x80807D8FU;
    constexpr std::uint32_t kTaskClass = 0x80807D95U;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> configs{};
    std::map<std::uint32_t, std::vector<std::uint32_t>> slots{};
    try {
        for (const squads::DescriptorFact& descriptor : facts.descriptors) {
            if (descriptor.slotIndex >= topology.slots.size()
                || topology.slots[descriptor.slotIndex].slotType != format::kObjectiveSlotType
                || descriptor.componentClass != format::kObjectiveComponentClass
                || descriptor.senseSchema != format::kObjectiveSenseSchema
                || descriptor.authSchema != format::kObjectiveAuthSchema) {
                continue;
            }
            const std::uint64_t descriptorKey =
                (static_cast<std::uint64_t>(descriptor.configTag) << 32U)
                | descriptor.descriptorOffset;
            auto found = configs.find(descriptorKey);
            if (found == configs.end()) {
                std::vector<std::byte> bytes{};
                std::uint32_t configClass = 0;
                std::size_t groupData = 0;
                std::size_t groupCount = 0;
                if (!read_tag(&packageContext, descriptor.configTag, bytes, configClass)
                    || !read_array(bytes,
                                   descriptor.descriptorOffset + kGroupsField,
                                   kRowStride,
                                   kGroupClass,
                                   groupData,
                                   groupCount)) {
                    return false;
                }
                std::vector<std::uint32_t> taskCounts{};
                for (std::size_t group = 0; group < groupCount; ++group) {
                    std::size_t taskData = 0;
                    std::size_t taskCount = 0;
                    if (!read_array(bytes,
                                    groupData + group * kRowStride + kTasksField,
                                    kRowStride,
                                    kTaskClass,
                                    taskData,
                                    taskCount)) {
                        return false;
                    }
                    taskCounts.push_back(static_cast<std::uint32_t>(taskCount));
                }
                found = configs.emplace(descriptorKey, std::move(taskCounts)).first;
            }
            const auto [slot, inserted] = slots.emplace(descriptor.slotIndex, found->second);
            if (!inserted && slot->second != found->second) {
                return false;
            }
            if (cancelled(packageContext.cancel, packageContext.cancelContext)) {
                return false;
            }
        }
        for (const auto& [slot, taskCounts] : slots) {
            for (std::uint32_t group = 0; group < taskCounts.size(); ++group) {
                output.combatObjectiveGroups.push_back({slot, group, taskCounts[group]});
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

/** Extracts localized dialogue aliases and safe authored directive elements. */
bool attach_authored_text(const topology_inventory::Snapshot& topology,
                          const squads::Facts& facts,
                          PackageContext& packageContext,
                          authored_scene::Snapshot& output) {
    struct CachedTag final {
        std::vector<std::byte> bytes{};
        std::uint32_t classId{};
    };
    std::unordered_map<std::uint32_t, CachedTag> cache{};
    auto package = [&](std::uint32_t tag, const CachedTag*& result) -> bool {
        const auto found = cache.find(tag);
        if (found != cache.end()) {
            result = &found->second;
            return true;
        }
        CachedTag row{};
        if (!read_tag(&packageContext, tag, row.bytes, row.classId)) {
            return false;
        }
        result = &cache.emplace(tag, std::move(row)).first->second;
        return true;
    };
    try {
        std::vector<AuthoredTextCandidate> dialogueCandidates{};
        std::vector<AuthoredDirectiveCandidate> directiveCandidates{};
        for (const squads::DescriptorFact& descriptor : facts.descriptors) {
            if (descriptor.slotIndex >= topology.slots.size()) {
                continue;
            }
            const std::uint32_t slotType = topology.slots[descriptor.slotIndex].slotType;
            if (slotType != format::kDialogueSlotType && slotType != 68U) {
                continue;
            }
            const CachedTag* config = nullptr;
            if (!package(descriptor.configTag, config) || config == nullptr) {
                continue;
            }
            const std::size_t field = static_cast<std::size_t>(descriptor.descriptorOffset)
                                      + (slotType == format::kDialogueSlotType ? 0x58U : 0x5CU);
            std::uint32_t resourceTag = 0;
            const CachedTag* resource = nullptr;
            if (!read_value(std::span(config->bytes), field, resourceTag)
                || !package(resourceTag, resource) || resource == nullptr) {
                continue;
            }
            const auto bytes = std::span(resource->bytes);
            if (slotType == format::kDialogueSlotType) {
                if (resource->classId != format::kDialogueAuthoredListClass) {
                    continue;
                }
                std::size_t groups = 0;
                std::size_t groupCount = 0;
                if (!read_array(
                        bytes, 0x18U, 16U, format::kDialogueGroupArrayClass, groups, groupCount)) {
                    continue;
                }
                std::size_t definitions = 0;
                std::size_t definitionCount = 0;
                if (!read_array(bytes,
                                8U,
                                8U,
                                format::kDialogueDefinitionArrayClass,
                                definitions,
                                definitionCount)) {
                    continue;
                }
                std::vector<dialogue_groups::Span> groupIndex{};
                if (!dialogue_groups::build(bytes, groups, groupCount, groupIndex)) {
                    continue;
                }
                for (std::size_t cue = 0; cue < definitionCount; ++cue) {
                    std::uint32_t definitionHash = 0;
                    if (!read_value(bytes, definitions + cue * 8U, definitionHash)
                        || definitionHash == 0 || definitionHash == 0x811C9DC5U) {
                        continue;
                    }
                    dialogue_groups::Span group{};
                    if (!dialogue_groups::find(groupIndex, definitionHash, group)) {
                        continue;
                    }
                    for (std::size_t offset = group.begin; offset + 8U <= group.end; offset += 4U) {
                        std::uint32_t containerTag = 0;
                        std::uint32_t stringHash = 0;
                        const CachedTag* container = nullptr;
                        if (!read_value(bytes, offset, containerTag)
                            || !read_value(bytes, offset + 4U, stringHash)
                            || containerTag < 0x80A12000U || containerTag >= 0xC0000000U
                            || !package(containerTag, container) || container == nullptr
                            || container->classId != display::kStringContainerClass) {
                            continue;
                        }
                        dialogueCandidates.push_back({{containerTag, stringHash},
                                                      descriptor.slotIndex,
                                                      static_cast<std::uint32_t>(cue),
                                                      definitionHash});
                    }
                }
            } else {
                if (resource->classId != 0x80804F72U) {
                    continue;
                }
                std::size_t entries = 0;
                std::size_t entryCount = 0;
                if (!read_array(bytes, 8U, 40U, 0x80804F74U, entries, entryCount)) {
                    continue;
                }
                for (std::size_t entry = 0; entry < entryCount; ++entry) {
                    const std::size_t row = entries + entry * 40U;
                    std::uint32_t nameHash = 0;
                    std::int64_t relative = 0;
                    std::size_t elements = 0;
                    std::uint64_t elementCount = 0;
                    std::uint32_t elementClass = 0;
                    if (!read_value(bytes, row, nameHash) || !read_value(bytes, row + 24U, relative)
                        || !add_relative(row + 24U, relative, elements)
                        || !read_value(bytes, elements, elementCount) || elementCount == 0
                        || elementCount > format::kAbsentIndex
                        || !read_value(bytes, elements + 8U, elementClass)
                        || elementClass != 0x80804F76U) {
                        continue;
                    }
                    const std::size_t data = elements + 16U;
                    if (data > bytes.size() || elementCount > (bytes.size() - data) / 36U) {
                        continue;
                    }
                    for (std::size_t element = 0; element < elementCount; ++element) {
                        const std::size_t elementRow = data + element * 36U;
                        AuthoredDirectiveCandidate candidate{};
                        candidate.slotIndex = descriptor.slotIndex;
                        candidate.nameHash = nameHash;
                        candidate.elementIndex = static_cast<std::int32_t>(element);
                        candidate.elementCount = static_cast<std::uint32_t>(elementCount);
                        if (!read_value(bytes, elementRow, candidate.title.containerTag)
                            || !read_value(bytes, elementRow + 4U, candidate.title.stringHash)
                            || !read_value(
                                bytes, elementRow + 8U, candidate.description.containerTag)
                            || !read_value(
                                bytes, elementRow + 12U, candidate.description.stringHash)) {
                            continue;
                        }
                        const CachedTag* title = nullptr;
                        const CachedTag* description = nullptr;
                        if (!package(candidate.title.containerTag, title) || title == nullptr
                            || title->classId != display::kStringContainerClass
                            || !package(candidate.description.containerTag, description)
                            || description == nullptr
                            || description->classId != display::kStringContainerClass) {
                            continue;
                        }
                        directiveCandidates.push_back(candidate);
                    }
                }
            }
        }
        std::vector<display::Reference> references{};
        references.reserve(dialogueCandidates.size() + directiveCandidates.size() * 2U);
        for (const AuthoredTextCandidate& row : dialogueCandidates) {
            references.push_back(row.reference);
        }
        for (const AuthoredDirectiveCandidate& row : directiveCandidates) {
            references.push_back(row.title);
            references.push_back(row.description);
        }
        if (references.empty()) {
            return true;
        }
        display::Snapshot names{};
        if (!display::resolve({&packageContext, &read_localized_tag, 0, 0}, references, names)
            || names.names.size() != references.size()) {
            return true;
        }
        for (std::size_t index = 0; index < dialogueCandidates.size(); ++index) {
            const display::Name& name = names.names[index];
            if (name.authoredEmpty || name.length == 0) {
                continue;
            }
            const std::string_view text(name.value.data(), name.length);
            char id[96]{};
            const AuthoredTextCandidate& candidate = dialogueCandidates[index];
            const int length = std::snprintf(id,
                                             sizeof id,
                                             "dialogue/%08x/%u/%08x/%08x",
                                             candidate.slotIndex,
                                             candidate.cueIndex,
                                             candidate.definitionHash,
                                             candidate.reference.stringHash);
            authored_scene::DialogueCueText row{};
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof id
                || !copy_text(std::string_view(id, static_cast<std::size_t>(length)), row.id)
                || !copy_text(text, row.text)) {
                continue;
            }
            row.slotIndex = candidate.slotIndex;
            row.cueIndex = candidate.cueIndex;
            row.definitionHash = candidate.definitionHash;
            row.containerTag = candidate.reference.containerTag;
            row.stringHash = candidate.reference.stringHash;
            output.dialogueCueTexts.push_back(row);
        }
        std::size_t resolved = dialogueCandidates.size();
        for (const AuthoredDirectiveCandidate& candidate : directiveCandidates) {
            const display::Name& title = names.names[resolved++];
            const display::Name& description = names.names[resolved++];
            if (title.authoredEmpty || title.length == 0 || description.authoredEmpty
                || description.length == 0) {
                continue;
            }
            char id[96]{};
            const int length = std::snprintf(id,
                                             sizeof id,
                                             "directive/%08x/%08x/%d",
                                             candidate.slotIndex,
                                             candidate.nameHash,
                                             candidate.elementIndex);
            authored_scene::DirectiveElement row{};
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof id
                || !copy_text(std::string_view(id, static_cast<std::size_t>(length)), row.id)
                || !copy_text(std::string_view(title.value.data(), title.length), row.title)
                || !copy_text(std::string_view(description.value.data(), description.length),
                              row.description)) {
                continue;
            }
            row.slotIndex = candidate.slotIndex;
            row.nameHash = candidate.nameHash;
            row.elementIndex = candidate.elementIndex;
            row.elementCount = candidate.elementCount;
            row.titleContainerTag = candidate.title.containerTag;
            row.titleStringHash = candidate.title.stringHash;
            row.descriptionContainerTag = candidate.description.containerTag;
            row.descriptionStringHash = candidate.description.stringHash;
            output.directiveElements.push_back(row);
        }
        auto dialogueLess = [](const auto& first, const auto& second) {
            return std::tie(first.slotIndex, first.cueIndex, first.definitionHash, first.stringHash)
                   < std::tie(
                       second.slotIndex, second.cueIndex, second.definitionHash, second.stringHash);
        };
        auto directiveLess = [](const auto& first, const auto& second) {
            return std::tie(first.slotIndex, first.nameHash, first.elementIndex)
                   < std::tie(second.slotIndex, second.nameHash, second.elementIndex);
        };
        std::sort(output.dialogueCueTexts.begin(), output.dialogueCueTexts.end(), dialogueLess);
        std::sort(output.directiveElements.begin(), output.directiveElements.end(), directiveLess);
        output.dialogueCueTexts.erase(std::unique(output.dialogueCueTexts.begin(),
                                                  output.dialogueCueTexts.end(),
                                                  [&](const auto& left, const auto& right) {
                                                      return !dialogueLess(left, right)
                                                             && !dialogueLess(right, left);
                                                  }),
                                      output.dialogueCueTexts.end());
        output.directiveElements.erase(std::unique(output.directiveElements.begin(),
                                                   output.directiveElements.end(),
                                                   [&](const auto& left, const auto& right) {
                                                       return !directiveLess(left, right)
                                                              && !directiveLess(right, left);
                                                   }),
                                       output.directiveElements.end());
        return true;
    } catch (...) {
        return false;
    }
}

/** Resolves the native type-53 authored list and attaches its exact bound to the SDK slot row. */
bool attach_dialogue_cue_counts(const topology_inventory::Snapshot& topology,
                                const squads::Facts& facts,
                                PackageContext& packageContext,
                                topology_enrichment::Snapshot& enrichment,
                                authored_scene::Snapshot& authoredRows) {
    if (topology.slots.size() != enrichment.slots.size()) {
        return false;
    }
    struct CachedTag final {
        std::vector<std::byte> bytes{};
        std::uint32_t classId{};
    };
    std::unordered_map<std::uint32_t, CachedTag> cache{};
    auto package = [&](std::uint32_t tag, const CachedTag*& output) -> bool {
        output = nullptr;
        const auto found = cache.find(tag);
        if (found != cache.end()) {
            output = &found->second;
            return true;
        }
        CachedTag row{};
        if (tag == 0 || tag == format::kAbsentIndex
            || !read_tag(&packageContext, tag, row.bytes, row.classId)) {
            return false;
        }
        const auto [inserted, accepted] = cache.emplace(tag, std::move(row));
        if (!accepted) {
            return false;
        }
        output = &inserted->second;
        return true;
    };

    try {
        cache.reserve(facts.descriptors.size());
        for (std::uint32_t slotRow = 0; slotRow < topology.slots.size(); ++slotRow) {
            if (topology.slots[slotRow].slotType != format::kDialogueSlotType) {
                continue;
            }
            topology_enrichment::Slot& enriched = enrichment.slots[slotRow];
            if (enriched.componentClass != format::kDialogueComponentClass
                || enriched.authSchema != format::kDialogueAuthSchema
                || (enriched.flags & format::kSlotSchemaJoinExact) == 0) {
                continue;
            }
            std::uint64_t agreedCount = 0;
            std::vector<dialogue_groups::Definition> agreedDefinitions{};
            bool sawDescriptor = false;
            bool resolved = true;
            for (const squads::DescriptorFact& descriptor : facts.descriptors) {
                if (descriptor.slotIndex != slotRow) {
                    continue;
                }
                sawDescriptor = true;
                const CachedTag* config = nullptr;
                if (!package(descriptor.configTag, config) || config == nullptr) {
                    resolved = false;
                    break;
                }
                const std::size_t listField = static_cast<std::size_t>(descriptor.descriptorOffset)
                                              + format::kDialogueAuthoredListRelativeOffset;
                std::uint32_t listTag = 0;
                if (!read_value(std::span(config->bytes), listField, listTag) || listTag == 0
                    || listTag == format::kAbsentIndex) {
                    resolved = false;
                    break;
                }
                const CachedTag* authored = nullptr;
                std::vector<dialogue_groups::Definition> definitions{};
                if (!package(listTag, authored) || authored == nullptr
                    || authored->classId != format::kDialogueAuthoredListClass
                    || !dialogue_groups::definitions(authored->bytes, definitions)
                    || definitions.empty()
                    || (agreedCount != 0 && agreedDefinitions != definitions)) {
                    resolved = false;
                    break;
                }
                agreedCount = definitions.size();
                agreedDefinitions = std::move(definitions);
            }
            if (cancelled(packageContext.cancel, packageContext.cancelContext)) {
                return false;
            }
            if (resolved && sawDescriptor && agreedCount != 0) {
                enriched.dialogueCueCount = static_cast<std::uint32_t>(agreedCount);
                enriched.flags |= format::kSlotDialogueCuesExact;
                for (std::uint32_t cue = 0; cue < agreedDefinitions.size(); ++cue) {
                    const auto& definition = agreedDefinitions[cue];
                    authoredRows.dialogueCues.push_back(
                        {slotRow, cue, definition.hash, definition.authoredWindowSeconds});
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

/** Flags the slot rows of the scenes the inventory found without a resource. */
bool attach_unresourced_scenes(const topology_inventory::Snapshot& topology,
                               const authored_scene::Snapshot& scenes,
                               topology_enrichment::Snapshot& enrichment) noexcept {
    if (enrichment.slots.size() != topology.slots.size()) {
        return false;
    }
    for (const std::uint32_t slotRow : scenes.unresourcedSlots) {
        if (slotRow >= topology.slots.size()
            || topology.slots[slotRow].slotType != format::kAuthoredSceneSlotType) {
            return false;
        }
        topology_enrichment::Slot& enriched = enrichment.slots[slotRow];
        if (enriched.componentClass != format::kAuthoredSceneComponentClass
            || (enriched.flags & format::kSlotSchemaJoinExact) == 0) {
            return false;
        }
        enriched.flags |= format::kSlotAuthoredSceneUnresourced;
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
