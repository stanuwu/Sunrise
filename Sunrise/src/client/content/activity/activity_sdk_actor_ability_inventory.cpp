#include "activity_sdk_actor_ability_inventory.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "scriptable_catalog_reference_scan.h"

namespace sunrise::client::content::activity::sdk_generation::actor_ability_inventory {
namespace {

namespace format = state::activity_sdk::format;
namespace references = scriptables::internal;
namespace tables = middleware::content::packages::tables;

// Ability definitions contain 32-byte groups and four-byte request keys.
constexpr std::uint32_t kDefinitionClass = 0x8080815FU;
constexpr std::uint32_t kGroupClass = 0x808081ACU;
constexpr std::uint32_t kRequestClass = 0x80800070U;
constexpr std::size_t kGroupsField = 32U;
constexpr std::size_t kGroupStride = 32U;
constexpr std::size_t kGroupHashOffset = 12U;
constexpr std::size_t kRequestsField = 16U;
// Logical Type 58 points name a resource whose float-selector array starts at +64.
constexpr std::uint32_t kPointSlotType = 58U;
constexpr std::uint32_t kPointComponentClass = 0x80807D9BU;
constexpr std::uint32_t kPointResourceClass = 0x8080834CU;
constexpr std::uint32_t kPointSelectorClass = 0x8080000FU;
constexpr std::size_t kPointResourceOffset = 88U;
constexpr std::size_t kPointSelectorsField = 64U;
// Type 48 AI point sets are one multi-slot table per object (component class 0x80808344). Its
// descriptor names no single slot, so the descriptor walk never reports them. The table pairs a
// ClientRef row (name pointer, ClientRef, flags) with a resource handle whose payload has the
// Type 58 point resource class, selectors at +64 included.
constexpr std::uint32_t kPointSetSlotType = 48U;
constexpr std::uint32_t kPointSetComponentClass = 0x80808344U;
constexpr std::uint32_t kPointSetRowClass = 0x80809A6DU;
constexpr std::uint32_t kPointSetResourceHandleClass = 0x8080834BU;
constexpr std::size_t kPointSetRowsField = 0x28U;
constexpr std::size_t kPointSetRowStride = 0x18U;
constexpr std::size_t kPointSetRowReference = 8U;
constexpr std::size_t kPointSetResourcesField = 0x38U;
// Runtime ability keys use the empty-name hash as the absent value.
constexpr std::uint32_t kAbsentName = 0x811C9DC5U;
// Logical Type 2 owns the actor controller.
constexpr std::uint32_t kCombatantSlotType = 2U;
// Package arrays place their elements after a 16-byte header.
constexpr std::size_t kArrayHeaderSize = 16U;

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
        || !read(bytes, field + sizeof rawCount, relative)) {
        return false;
    }
    count = static_cast<std::uint32_t>(rawCount);
    if (count == 0 && relative == 0) {
        data = 0;
        return true;
    }
    const auto base = static_cast<std::int64_t>(field + sizeof rawCount);
    if ((relative > 0 && base > (std::numeric_limits<std::int64_t>::max)() - relative)
        || (relative < 0 && relative < -base)) {
        return false;
    }
    const auto header = static_cast<std::size_t>(base + relative);
    std::uint64_t repeated = 0;
    std::uint32_t actualClass = 0;
    if (!read(bytes, header, repeated) || repeated != count
        || !read(bytes, header + sizeof repeated, actualClass) || actualClass != expectedClass
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
 * Reads the selector count of one point resource.
 * @return False when the tag is not an exact point resource.
 */
bool point_selectors(Packages& packages, std::uint32_t resource, std::uint32_t& count) {
    const Package* point = packages.get(resource);
    std::size_t data = 0;
    return point != nullptr && point->classId == kPointResourceClass
           && array(
               point->bytes, kPointSelectorsField, sizeof(float), kPointSelectorClass, data, count);
}

/**
 * Adds every Type 48 point set one config authors to `targets`.
 * @param packages Cached installed package reader.
 * @param configTag Placed config to scan.
 * @param slots Type 48 slot rows by their exact ClientRef.
 * @param targets Receives one target per proved slot.
 * @param invalid Receives slots whose rows disagree or whose resource is malformed.
 * @return False only when the config cannot be read.
 */
bool point_sets(Packages& packages,
                std::uint32_t configTag,
                const std::map<Source, std::uint32_t>& slots,
                std::map<std::uint32_t, format::ActorAbilityTarget>& targets,
                std::set<std::uint32_t>& invalid) {
    const Package* config = packages.get(configTag);
    if (config == nullptr) {
        return false;
    }
    const std::span<const std::byte> bytes = config->bytes;
    for (std::size_t offset = 0; offset + 3 * sizeof(std::uint32_t) <= bytes.size();
         offset += tables::kDescriptorStep) {
        std::uint32_t own = 0;
        std::uint32_t component = 0;
        std::uint32_t mark = 0;
        if (!read(bytes, offset, own) || own != configTag
            || !read(bytes, offset + sizeof own, component) || component != kPointSetComponentClass
            || !read(bytes, offset + 2 * sizeof own, mark) || mark != tables::kDescriptorMark) {
            continue;
        }
        std::size_t rows = 0;
        std::uint32_t rowCount = 0;
        std::size_t resources = 0;
        std::uint32_t resourceCount = 0;
        if (!array(bytes,
                   offset + kPointSetRowsField,
                   kPointSetRowStride,
                   kPointSetRowClass,
                   rows,
                   rowCount)
            || !array(bytes,
                      offset + kPointSetResourcesField,
                      sizeof(std::uint32_t),
                      kPointSetResourceHandleClass,
                      resources,
                      resourceCount)
            || rowCount != resourceCount) {
            continue;
        }
        for (std::uint32_t index = 0; index < rowCount; ++index) {
            const std::size_t row = rows + index * kPointSetRowStride + kPointSetRowReference;
            std::uint32_t key = 0;
            std::uint16_t type = 0;
            std::uint16_t slotIndex = 0;
            std::uint32_t resource = 0;
            if (!read(bytes, row, key) || !read(bytes, row + sizeof key, type)
                || !read(bytes, row + sizeof key + sizeof type, slotIndex)
                || !read(bytes, resources + index * sizeof resource, resource)) {
                continue;
            }
            // The ClientRef's slot type is a byte; the byte after it is padding.
            const auto found =
                slots.find({key, static_cast<std::uint16_t>(type & 0xFFU), slotIndex});
            if (found == slots.end()) {
                continue;
            }
            std::uint32_t count = 0;
            if (!point_selectors(packages, resource, count)) {
                invalid.insert(found->second);
                continue;
            }
            const format::ActorAbilityTarget target{found->second, resource, count};
            const auto [existing, inserted] = targets.emplace(found->second, target);
            if (!inserted && existing->second != target) {
                invalid.insert(found->second);
            }
        }
    }
    return true;
}

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
                      sizeof(std::uint32_t),
                      kRequestClass,
                      keys,
                      keyCount)) {
            return false;
        }
        for (std::uint32_t key = 0; key < keyCount; ++key) {
            std::uint32_t request = 0;
            if (!read(package->bytes, keys + key * sizeof request, request)) {
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
                              sizeof(float),
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
        // Type 48 point sets: no descriptor names them, so read every placed config once.
        std::map<Source, std::uint32_t> pointSetSlots{};
        for (std::uint32_t row = 0; row < topology.slots.size(); ++row) {
            const auto& slot = topology.slots[row];
            if (slot.slotType == kPointSetSlotType && slot.objectIndex < topology.objects.size()) {
                pointSetSlots.emplace(Source{topology.objects[slot.objectIndex].objectKey,
                                             static_cast<std::uint16_t>(slot.slotType),
                                             static_cast<std::uint16_t>(slot.slotIndex)},
                                      row);
            }
        }
        if (!pointSetSlots.empty()) {
            std::set<std::uint32_t> configs{};
            for (const auto& occurrence : facts.configOccurrences) {
                configs.insert(occurrence.configTag);
            }
            for (const auto configTag : configs) {
                if (!point_sets(packages, configTag, pointSetSlots, targets, invalidTargets)) {
                    return false;
                }
            }
        }
        std::map<std::uint32_t, std::set<std::uint32_t>> definitions{};
        for (const auto& binding : output.sequenceBindings) {
            if (binding.localTableIndex < output.sequenceTables.size()) {
                definitions[binding.actorClassIndex].insert(
                    output.sequenceTables[binding.localTableIndex].definitionTag);
            }
        }
        std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> names{};
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
            if (actor.invalid || actor.row == format::kAbsentIndex
                || definitions[actor.row].size() != 1) {
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
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::actor_ability_inventory
