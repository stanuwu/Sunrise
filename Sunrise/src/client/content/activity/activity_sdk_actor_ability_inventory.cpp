#include "activity_sdk_actor_ability_inventory.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/layout.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808000.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808081.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808082.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808083.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808097.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809c.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809f.h"
#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "scriptable_catalog_reference_scan.h"

namespace sunrise::client::content::activity::sdk_generation::actor_ability_inventory {
namespace {

namespace format = state::activity_sdk::format;
namespace references = scriptables::internal;
namespace tables = middleware::content::packages::tables;
namespace generated = middleware::content::packages::schema::generated;
namespace layout = middleware::content::packages::reader::layout;
using ArrayHeader = layout::TypedArrayHeader;

// Ability definitions contain groups of request keys.
constexpr std::uint32_t kDefinitionClass = generated::ActorStateMachineDefinition::kClass;
constexpr std::uint32_t kGroupClass = generated::StateMachineGroup::kClass;
constexpr std::uint32_t kRequestClass = generated::SU32HashElement::kClass;
constexpr std::size_t kGroupsField = offsetof(generated::ActorStateMachineDefinition, abilities);
constexpr std::size_t kGroupStride = sizeof(generated::StateMachineGroup);
constexpr std::size_t kGroupHashOffset = offsetof(generated::StateMachineGroup, groupHash);
constexpr std::size_t kRequestsField = offsetof(generated::StateMachineGroup, typeNames);
constexpr std::size_t kRequestStride = sizeof(generated::SU32HashElement);
constexpr std::size_t kRequestKeyOffset = offsetof(generated::SU32HashElement, keyHash);
// Logical Type 58 points name a resource that holds a float-selector array.
constexpr std::uint32_t kPointSlotType = 58U;
constexpr std::uint32_t kPointComponentClass = generated::AbilityPointComponent::kClass;
constexpr std::uint32_t kPointResourceClass = generated::AbilityPointResource::kClass;
constexpr std::uint32_t kPointSelectorClass = generated::SF32Column::kClass;
constexpr std::size_t kPointResourceOffset =
    offsetof(generated::AbilityPointSlotDefinitionType58, resource);
constexpr std::size_t kPointSelectorsField =
    offsetof(generated::AbilityPointResource, floatSelectors);
constexpr std::size_t kPointSelectorStride = sizeof(generated::SF32Column);
// Runtime ability keys use the empty-name hash as the absent value.
constexpr std::uint32_t kAbsentName = 0x811C9DC5U;
// Logical Type 2 owns the actor controller.
constexpr std::uint32_t kCombatantSlotType = 2U;
// Package arrays place their elements after the typed-array header.
constexpr std::size_t kArrayHeaderSize = ArrayHeader::kSize;
// An actor's behaviour program sits beside its local definition in the AI source. It holds one
// group per shipped program, and each group a table of temperament ids sorted for the client's
// binary search; the client ignores an id the table does not hold.
constexpr std::size_t kProgramField = offsetof(generated::ActorGroupSource, unk2fc);
constexpr std::uint32_t kProgramClass = generated::C80808162::kClass;
constexpr std::size_t kProgramGroupsField = offsetof(generated::C80808162, h0dce735b);
constexpr std::uint32_t kProgramGroupClass = generated::C80808164::kClass;
constexpr std::size_t kProgramGroupStride = sizeof(generated::C80808164);
constexpr std::size_t kTemperamentsField = offsetof(generated::C80808164, temperaments);
constexpr std::uint32_t kTemperamentClass = generated::C808081b4::kClass;
constexpr std::size_t kTemperamentStride = sizeof(generated::C808081b4);
constexpr std::size_t kTemperamentIdOffset = offsetof(generated::C808081b4, unk0);
// An actor's object channels are the rows of its value provider. The source lists, by row, the
// rows nothing on the actor binds or computes: those a server write sets.
constexpr std::uint32_t kEntityClass = generated::SEntity::kClass;
constexpr std::size_t kComponentsField = offsetof(generated::SEntity, components);
constexpr std::uint32_t kComponentClass = generated::SPatternComponent::kClass;
constexpr std::size_t kComponentStride = sizeof(generated::SPatternComponent);
constexpr std::size_t kComponentConfigOffset = offsetof(generated::SPatternComponent, config);
constexpr std::uint32_t kConfigClass = generated::SEntityConfig::kClass;
constexpr std::uint32_t kProviderClass = generated::UnknownValueProviderPrimary::kClass;
constexpr std::uint32_t kProviderSourceClass = generated::SVariableStore::kClass;
constexpr std::size_t kProviderRowsField = offsetof(generated::SVariableStore, providerRows);
constexpr std::uint32_t kProviderRowClass = generated::UnknownValueProviderRow::kClass;
constexpr std::size_t kProviderRowStride = sizeof(generated::UnknownValueProviderRow);
constexpr std::size_t kProviderRowNameOffset =
    offsetof(generated::UnknownValueProviderRow, rowName);
constexpr std::size_t kExternalRowsField = offsetof(generated::SVariableStore, h52a3e673);
constexpr std::uint32_t kExternalRowClass = generated::UnknownValueProviderSource::kClass;
constexpr std::size_t kExternalRowStride = sizeof(generated::UnknownValueProviderSource);
constexpr std::size_t kExternalRowIndexOffset =
    offsetof(generated::UnknownValueProviderSource, rowIndex);
// The AI source also lists, by name, the object channels its behaviour program reads.
constexpr std::size_t kProgramChannelsField = offsetof(generated::ActorGroupSource, h86374c3c);
constexpr std::uint32_t kProgramChannelClass = generated::C80807ee9::kClass;
constexpr std::size_t kProgramChannelStride = sizeof(generated::C80807ee9);
constexpr std::size_t kProgramChannelNameOffset =
    offsetof(generated::C80807ee9, h01041631)
    + offsetof(generated::SModelValueBankResource, channelHashReloading0xecde609e);

/** Reads one bounded package scalar. @return False without changing the output on failure. */
template <typename Value>
bool read(std::span<const std::byte> bytes, std::size_t offset, Value& output) noexcept {
    if (offset > bytes.size() || sizeof output > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&output, bytes.data() + offset, sizeof output);
    return true;
}

/**
 * Reads one bounded package array.
 * @param bytes Package entry.
 * @param field Count and relative-pointer field.
 * @param stride Size of one element.
 * @param expectedClass Required element class.
 * @param data Receives the first element offset.
 * @param count Receives the element count.
 * @return False when the count, class or extent is invalid.
 */
bool array(std::span<const std::byte> bytes,
           std::size_t field,
           std::size_t stride,
           std::uint32_t expectedClass,
           std::size_t& data,
           std::uint32_t& count) noexcept {
    std::uint64_t rawCount = 0;
    std::int64_t relative = 0;
    if (!read(bytes, field, rawCount) || rawCount > format::kAbsentIndex
        || !read(bytes, field + layout::kArrayFieldRelative, relative)) {
        return false;
    }
    count = static_cast<std::uint32_t>(rawCount);
    if (count == 0 && relative == 0) {
        data = 0;
        return true;
    }
    const auto base = static_cast<std::int64_t>(field + layout::kArrayFieldRelative);
    if ((relative > 0 && base > (std::numeric_limits<std::int64_t>::max)() - relative)
        || (relative < 0 && relative < -base)) {
        return false;
    }
    const auto header = static_cast<std::size_t>(base + relative);
    std::uint64_t repeated = 0;
    std::uint32_t actualClass = 0;
    if (!read(bytes, header, repeated) || repeated != count
        || !read(bytes, header + ArrayHeader::kClass, actualClass) || actualClass != expectedClass
        || header > bytes.size() || kArrayHeaderSize > bytes.size() - header) {
        return false;
    }
    data = header + kArrayHeaderSize;
    return count <= (bytes.size() - data) / stride;
}

struct Package final {
    std::vector<std::byte> bytes{};
    std::uint32_t classId{};
};

struct Packages final {
    squad_inventory::TagReader reader{};
    void* context{};
    std::map<std::uint32_t, Package> rows{};

    /** @param tag Installed entry identity. @return The entry, retained until extraction ends. */
    const Package* get(std::uint32_t tag) {
        const auto found = rows.find(tag);
        if (found != rows.end()) {
            return &found->second;
        }
        Package value{};
        if (!reader(context, tag, value.bytes, value.classId)) {
            return nullptr;
        }
        return &rows.emplace(tag, std::move(value)).first->second;
    }
};

struct Actor final {
    std::uint32_t row{format::kAbsentIndex};
    bool invalid{};

    /** Different classes or a missing class leave the owner ambiguous. */
    void accept(std::uint32_t candidate) noexcept {
        invalid = invalid || candidate == format::kAbsentIndex
                  || (row != format::kAbsentIndex && row != candidate);
        row = candidate;
    }
};

using Source = std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>;
using ScopedSource = std::tuple<std::uint32_t, Source>;

/**
 * Reads only keys from an exact local actor definition.
 * @param packages Cached installed package reader.
 * @param definition Actor-local definition tag.
 * @param output Receives sorted group and request keys.
 * @return False when the authored table is malformed.
 */
bool requests(Packages& packages,
              std::uint32_t definition,
              std::vector<std::pair<std::uint32_t, std::uint32_t>>& output) {
    const Package* package = packages.get(definition);
    std::size_t data = 0;
    std::uint32_t count = 0;
    if (package == nullptr || package->classId != kDefinitionClass
        || !array(package->bytes, kGroupsField, kGroupStride, kGroupClass, data, count)) {
        return false;
    }
    std::set<std::uint32_t> groups{};
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t row = data + index * kGroupStride;
        std::uint32_t group = 0;
        std::size_t keys = 0;
        std::uint32_t keyCount = 0;
        if (!read(package->bytes, row + kGroupHashOffset, group) || group == 0
            || group == kAbsentName || !groups.insert(group).second
            || !array(package->bytes,
                      row + kRequestsField,
                      kRequestStride,
                      kRequestClass,
                      keys,
                      keyCount)) {
            return false;
        }
        for (std::uint32_t key = 0; key < keyCount; ++key) {
            std::uint32_t request = 0;
            if (!read(package->bytes, keys + key * kRequestStride + kRequestKeyOffset, request)) {
                return false;
            }
            if (request != 0 && request != kAbsentName && request != format::kAbsentIndex) {
                output.emplace_back(group, request);
            }
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return true;
}

/**
 * Reads every temperament id of one behaviour program.
 * @param output Receives the ids, sorted and unique.
 * @return False when the program is not one or its tables are malformed.
 */
bool program_temperaments(Packages& packages,
                          std::uint32_t programTag,
                          std::vector<std::uint32_t>& output) {
    output.clear();
    const Package* program = packages.get(programTag);
    std::size_t groups = 0;
    std::uint32_t groupCount = 0;
    if (program == nullptr || program->classId != kProgramClass
        || !array(program->bytes,
                  kProgramGroupsField,
                  kProgramGroupStride,
                  kProgramGroupClass,
                  groups,
                  groupCount)) {
        return false;
    }
    for (std::uint32_t group = 0; group < groupCount; ++group) {
        std::size_t rows = 0;
        std::uint32_t count = 0;
        if (!array(program->bytes,
                   groups + group * kProgramGroupStride + kTemperamentsField,
                   kTemperamentStride,
                   kTemperamentClass,
                   rows,
                   count)) {
            return false;
        }
        for (std::uint32_t row = 0; row < count; ++row) {
            std::uint32_t id = 0;
            if (!read(program->bytes, rows + row * kTemperamentStride + kTemperamentIdOffset, id)) {
                return false;
            }
            if (id != 0) {
                output.push_back(id);
            }
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return true;
}

/**
 * Reads the externally set rows of every value provider one actor class names.
 * @param output Receives (provider config, row name) pairs, sorted and unique.
 * @return False when the actor or a provider is malformed.
 */
bool actor_channels(Packages& packages,
                    std::uint32_t actorTag,
                    std::vector<std::pair<std::uint32_t, std::uint32_t>>& output) {
    output.clear();
    const Package* actor = packages.get(actorTag);
    std::size_t components = 0;
    std::uint32_t componentCount = 0;
    if (actor == nullptr || actor->classId != kEntityClass
        || !array(actor->bytes,
                  kComponentsField,
                  kComponentStride,
                  kComponentClass,
                  components,
                  componentCount)) {
        return false;
    }
    for (std::uint32_t component = 0; component < componentCount; ++component) {
        std::uint32_t configTag = 0;
        if (!read(actor->bytes,
                  components + component * kComponentStride + kComponentConfigOffset,
                  configTag)) {
            return false;
        }
        const Package* config = packages.get(configTag);
        tables::ComponentPair pair{};
        if (config == nullptr || config->classId != kConfigClass
            || !tables::config_component_pair(config->bytes, configTag, pair)) {
            return false;
        }
        if (pair.primaryClass != kProviderClass || pair.secondaryClass != kProviderSourceClass) {
            continue;
        }
        std::size_t rows = 0;
        std::size_t external = 0;
        std::uint32_t rowCount = 0;
        std::uint32_t externalCount = 0;
        if (!array(config->bytes,
                   pair.secondaryOffset + kProviderRowsField,
                   kProviderRowStride,
                   kProviderRowClass,
                   rows,
                   rowCount)
            || !array(config->bytes,
                      pair.secondaryOffset + kExternalRowsField,
                      kExternalRowStride,
                      kExternalRowClass,
                      external,
                      externalCount)) {
            return false;
        }
        for (std::uint32_t entry = 0; entry < externalCount; ++entry) {
            std::int32_t row = -1;
            std::uint32_t name = 0;
            if (!read(config->bytes,
                      external + entry * kExternalRowStride + kExternalRowIndexOffset,
                      row)
                || row < 0 || static_cast<std::uint32_t>(row) >= rowCount
                || !read(config->bytes,
                         rows + static_cast<std::size_t>(row) * kProviderRowStride
                             + kProviderRowNameOffset,
                         name)) {
                return false;
            }
            if (name != 0 && name != kAbsentName) {
                output.emplace_back(configTag, name);
            }
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return true;
}

/**
 * Reads the one behaviour program the AI components of an actor class name.
 * @param output Receives zero when the class names none or more than one.
 * @return False when a referenced AI source cannot be read.
 */
bool actor_program(Packages& packages,
                   const actor_rsat_inventory::Snapshot& snapshot,
                   std::uint32_t actorClass,
                   std::uint32_t& output) {
    output = 0;
    std::set<std::uint32_t> programs{};
    bool absent = false;
    for (const auto& binding : snapshot.sequenceBindings) {
        if (binding.actorClassIndex != actorClass) {
            continue;
        }
        const Package* config = packages.get(binding.configTag);
        std::uint32_t program = 0;
        if (config == nullptr
            || !read(config->bytes,
                     static_cast<std::size_t>(binding.sourceOffset) + kProgramField,
                     program)) {
            return false;
        }
        absent = absent || program == 0 || program == format::kAbsentIndex;
        programs.insert(program);
    }
    if (!absent && programs.size() == 1) {
        output = *programs.begin();
    }
    return true;
}

/**
 * Reads the object channels the AI components of an actor class read.
 * @param output Receives (AI config, channel name) pairs, sorted and unique.
 * @return False when an AI source's channel list is malformed.
 */
bool program_channels(Packages& packages,
                      const actor_rsat_inventory::Snapshot& snapshot,
                      std::uint32_t actorClass,
                      std::vector<std::pair<std::uint32_t, std::uint32_t>>& output) {
    output.clear();
    for (const auto& binding : snapshot.sequenceBindings) {
        if (binding.actorClassIndex != actorClass) {
            continue;
        }
        const Package* config = packages.get(binding.configTag);
        std::size_t rows = 0;
        std::uint32_t count = 0;
        if (config == nullptr
            || !array(config->bytes,
                      static_cast<std::size_t>(binding.sourceOffset) + kProgramChannelsField,
                      kProgramChannelStride,
                      kProgramChannelClass,
                      rows,
                      count)) {
            return false;
        }
        for (std::uint32_t row = 0; row < count; ++row) {
            std::uint32_t name = 0;
            if (!read(config->bytes,
                      rows + row * kProgramChannelStride + kProgramChannelNameOffset,
                      name)) {
                return false;
            }
            if (name != 0 && name != kAbsentName) {
                output.emplace_back(binding.configTag, name);
            }
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return true;
}

/** Reports a failed control table read without publishing an incomplete catalog. */
[[nodiscard]] bool invalid_controls(std::uint32_t tag, const char* field) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=activity_sdk stage=actor_controls tag=0x%08X field=%s",
                      tag,
                      field);
    return false;
}

/**
 * Adds each slot's temperaments and each actor slot's writable channels: its value provider's
 * external rows and its behaviour program's inputs, one row per name.
 * @param slotClasses The actor classes each Type 1 or Type 2 slot spawns.
 * @param actorSlots The Type 2 slots among them.
 * @return False when a referenced control table cannot be read.
 */
bool actor_controls(Packages& packages,
                    const std::map<std::uint32_t, std::set<std::uint32_t>>& slotClasses,
                    const std::set<std::uint32_t>& actorSlots,
                    actor_rsat_inventory::Snapshot& output) {
    std::map<std::uint32_t, std::uint32_t> programs{};
    std::map<std::uint32_t, std::vector<std::uint32_t>> programTables{};
    output.actorTemperaments.clear();
    output.actorChannels.clear();
    for (const auto& [slotRow, classes] : slotClasses) {
        std::set<std::pair<std::uint32_t, std::uint32_t>> ids{};
        for (const std::uint32_t actorClass : classes) {
            auto found = programs.find(actorClass);
            if (found == programs.end()) {
                std::uint32_t program = 0;
                if (!actor_program(packages, output, actorClass, program)) {
                    return invalid_controls(output.actorClasses[actorClass].definitionTag,
                                            "program");
                }
                found = programs.emplace(actorClass, program).first;
            }
            const std::uint32_t program = found->second;
            if (program == 0) {
                continue;
            }
            auto table = programTables.find(program);
            if (table == programTables.end()) {
                std::vector<std::uint32_t> programIds{};
                if (!program_temperaments(packages, program, programIds)) {
                    return invalid_controls(program, "temperaments");
                }
                table = programTables.emplace(program, std::move(programIds)).first;
            }
            for (const std::uint32_t id : table->second) {
                ids.emplace(id, program);
            }
        }
        for (const auto& [id, program] : ids) {
            output.actorTemperaments.push_back({slotRow, program, id});
        }
        if (!actorSlots.contains(slotRow) || classes.size() != 1) {
            continue;
        }
        const std::uint32_t actorClass = *classes.begin();
        if (actorClass >= output.actorClasses.size()) {
            continue;
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> providerChannels{};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> programInputs{};
        if (!actor_channels(
                packages, output.actorClasses[actorClass].definitionTag, providerChannels)) {
            return invalid_controls(output.actorClasses[actorClass].definitionTag,
                                    "provider_channels");
        }
        if (!program_channels(packages, output, actorClass, programInputs)) {
            return invalid_controls(output.actorClasses[actorClass].definitionTag,
                                    "program_channels");
        }
        // A provider row names its own record; a program input the provider also lists keeps it.
        std::map<std::uint32_t, std::uint32_t> sources{};
        for (const auto& [source, name] : providerChannels) {
            sources.emplace(name, source);
        }
        for (const auto& [source, name] : programInputs) {
            sources.emplace(name, source);
        }
        for (const auto& [name, source] : sources) {
            output.actorChannels.push_back({slotRow, source, name});
        }
    }
    const auto byId = [](const auto& left, const auto& right) {
        return std::tie(left.slotIndex, left.id) < std::tie(right.slotIndex, right.id);
    };
    const auto byName = [](const auto& left, const auto& right) {
        return std::tie(left.slotIndex, left.nameHash) < std::tie(right.slotIndex, right.nameHash);
    };
    std::sort(output.actorTemperaments.begin(), output.actorTemperaments.end(), byId);
    std::sort(output.actorChannels.begin(), output.actorChannels.end(), byName);
    return true;
}

} // namespace

/**
 * Requires the same actor class in every source scenario before exposing its ability keys.
 * @param topology Complete slot and occurrence identities.
 * @param facts Exact descriptor sources.
 * @param squads Linked authored members.
 * @param reader Installed package reader.
 * @param context Package reader state.
 * @param output Receives exact ability rows and point bounds.
 * @return False when package data cannot be read safely.
 */
bool build(const topology_inventory::Snapshot& topology,
           const squad_inventory::Facts& facts,
           const squad_inventory::Snapshot& squads,
           squad_inventory::TagReader reader,
           void* context,
           actor_rsat_inventory::Snapshot& output) noexcept {
    if (reader == nullptr) {
        return false;
    }
    try {
        Packages packages{reader, context};
        std::map<ScopedSource, Actor> actors{};
        for (const auto& squad : squads.squads) {
            const auto& slot = topology.slots[squad.slotIndex];
            const Source source{topology.objects[squad.objectIndex].objectKey,
                                static_cast<std::uint16_t>(slot.slotType),
                                static_cast<std::uint16_t>(slot.slotIndex)};
            Actor& actor = actors[{squad.scenarioIndex, source}];
            for (std::uint32_t index = 0; index < squad.members.count; ++index) {
                const auto& member = squads.members[squad.members.first + index];
                actor.accept((member.flags & format::kSquadMemberActorClassExact) != 0
                                 ? member.actorClassIndex
                                 : format::kAbsentIndex);
            }
        }
        std::map<std::uint32_t, std::set<std::uint32_t>> scenarios{};
        for (const auto& occurrence : topology.occurrences) {
            scenarios[occurrence.objectIndex].insert(occurrence.scenarioIndex);
        }
        std::map<std::uint32_t, std::set<Source>> parents{};
        std::vector<const squad_inventory::SlotSchemaFact*> schemas(topology.slots.size());
        for (const auto& schema : facts.slotSchemas) {
            schemas[schema.slotIndex] = &schema;
        }
        std::set<std::uint32_t> invalidParents{};
        std::map<std::uint32_t, format::ActorAbilityTarget> targets{};
        std::set<std::uint32_t> invalidTargets{};
        for (const auto& descriptor : facts.descriptors) {
            const auto& slot = topology.slots[descriptor.slotIndex];
            if (slot.slotType != kCombatantSlotType && slot.slotType != kPointSlotType) {
                continue;
            }
            const auto* schema = schemas[descriptor.slotIndex];
            if (schema == nullptr || !schema->exact
                || schema->componentClass != descriptor.componentClass
                || schema->senseSchema != descriptor.senseSchema
                || schema->authSchema != descriptor.authSchema
                || (slot.slotType == kPointSlotType
                    && (descriptor.componentClass != kPointComponentClass
                        || descriptor.senseSchema != format::kAbsentIndex
                        || descriptor.authSchema != format::kAbsentIndex))) {
                (slot.slotType == kPointSlotType ? invalidTargets : invalidParents)
                    .insert(descriptor.slotIndex);
                continue;
            }
            const Package* config = packages.get(descriptor.configTag);
            if (config == nullptr) {
                return false;
            }
            if (slot.slotType == kPointSlotType) {
                std::uint32_t resource = 0;
                std::size_t data = 0;
                std::uint32_t count = 0;
                const Package* point = nullptr;
                if (!read(
                        config->bytes, descriptor.descriptorOffset + kPointResourceOffset, resource)
                    || (point = packages.get(resource)) == nullptr
                    || point->classId != kPointResourceClass
                    || !array(point->bytes,
                              kPointSelectorsField,
                              kPointSelectorStride,
                              kPointSelectorClass,
                              data,
                              count)) {
                    invalidTargets.insert(descriptor.slotIndex);
                    continue;
                }
                const format::ActorAbilityTarget target{descriptor.slotIndex, resource, count};
                const auto [found, inserted] = targets.emplace(descriptor.slotIndex, target);
                if (!inserted && found->second != target) {
                    invalidTargets.insert(descriptor.slotIndex);
                }
                continue;
            }
            const tables::SlotDescriptor source{descriptor.configTag,
                                                descriptor.componentClass,
                                                descriptor.senseSchema,
                                                descriptor.authSchema,
                                                descriptor.descriptorOffset,
                                                0,
                                                static_cast<std::uint16_t>(slot.slotType),
                                                static_cast<std::uint16_t>(slot.slotIndex)};
            references::RawReference parent{};
            if (!references::read_type2_squad_reference(config->bytes, source, parent)) {
                invalidParents.insert(descriptor.slotIndex);
                continue;
            }
            parents[descriptor.slotIndex].emplace(
                parent.targetKey, parent.targetType, parent.targetIndex);
        }
        std::map<std::uint32_t, std::set<std::uint32_t>> definitions{};
        for (const auto& binding : output.sequenceBindings) {
            if (binding.localTableIndex < output.sequenceTables.size()) {
                definitions[binding.actorClassIndex].insert(
                    output.sequenceTables[binding.localTableIndex].definitionTag);
            }
        }
        std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> names{};
        // The classes each squad slot's members spawn as, and each actor slot's one class.
        std::map<std::uint32_t, std::set<std::uint32_t>> slotClasses{};
        std::set<std::uint32_t> actorSlots{};
        for (const auto& squad : squads.squads) {
            for (std::uint32_t index = 0; index < squad.members.count; ++index) {
                const auto& member = squads.members[squad.members.first + index];
                if ((member.flags & format::kSquadMemberActorClassExact) != 0) {
                    slotClasses[squad.slotIndex].insert(member.actorClassIndex);
                }
            }
        }
        output.actorAbilities.clear();
        for (const auto& [slotRow, source] : parents) {
            if (source.size() != 1 || invalidParents.contains(slotRow)) {
                continue;
            }
            Actor actor{};
            for (const auto scenario : scenarios[topology.slots[slotRow].objectIndex]) {
                const auto found = actors.find({scenario, *source.begin()});
                actor.accept(found == actors.end() || found->second.invalid ? format::kAbsentIndex
                                                                            : found->second.row);
            }
            if (actor.invalid || actor.row == format::kAbsentIndex) {
                continue;
            }
            slotClasses[slotRow] = {actor.row};
            actorSlots.insert(slotRow);
            if (definitions[actor.row].size() != 1) {
                continue;
            }
            const auto definition = *definitions[actor.row].begin();
            auto found = names.find(definition);
            if (found == names.end()) {
                std::vector<std::pair<std::uint32_t, std::uint32_t>> keys{};
                if (!requests(packages, definition, keys)) {
                    return false;
                }
                found = names.emplace(definition, std::move(keys)).first;
            }
            for (const auto& [group, request] : found->second) {
                output.actorAbilities.push_back({slotRow, actor.row, definition, group, request});
            }
        }
        output.actorAbilityTargets.clear();
        for (const auto& [slot, target] : targets) {
            if (!invalidTargets.contains(slot)) {
                output.actorAbilityTargets.push_back(target);
            }
        }
        return actor_controls(packages, slotClasses, actorSlots, output);
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::actor_ability_inventory
