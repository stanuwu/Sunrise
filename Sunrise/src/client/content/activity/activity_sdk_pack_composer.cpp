#include "activity_sdk_pack_composer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../state/build_data/scriptables/inline_name_evidence.h"
#include "activity_sdk_activity_enrichment_inventory.h"
#include "activity_sdk_activity_inventory.h"
#include "activity_sdk_actor_rsat_inventory.h"
#include "activity_sdk_authored_scene_inventory.h"
#include "activity_sdk_behavior_inventory.h"
#include "activity_sdk_pack_composer_internal.h"
#include "activity_sdk_policy_inventory.h"
#include "activity_sdk_squad_inventory.h"
#include "activity_sdk_topology_enrichment.h"
#include "activity_sdk_topology_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::pack_composer {
namespace {

namespace evidence = state::build_data::scriptables::inline_name_evidence;

/** Format-v9 state rows use bits zero and one for enabled and extraction-complete state. */
constexpr std::uint32_t kStateEnabled = 0x1U;
constexpr std::uint32_t kStateExtractionComplete = 0x2U;
static_assert((kStateEnabled | kStateExtractionComplete) == format::kStateFlagMask);

using detail::PreparedRanges;

/** UTF-8 text is ordered by bytes, independent of the signedness of `char`. */
[[nodiscard]] bool byte_less(std::string_view left, std::string_view right) noexcept {
    const std::size_t common = (std::min)(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
        const auto leftByte = static_cast<std::uint8_t>(left[index]);
        const auto rightByte = static_cast<std::uint8_t>(right[index]);
        if (leftByte != rightByte) {
            return leftByte < rightByte;
        }
    }
    return left.size() < right.size();
}

/** @return True for an empty value or one canonical UTF-8 value without an embedded null. */
[[nodiscard]] bool valid_text(std::string_view value) noexcept {
    return value.empty()
           || (value.find('\0') == std::string_view::npos
               && evidence::valid_utf8(
                   std::as_bytes(std::span<const char>(value.data(), value.size()))));
}

template <typename Text>
[[nodiscard]] bool text_view(const Text& input, std::string_view& output) noexcept {
    output = {};
    if (input.length >= input.value.size() || input.value[input.length] != '\0') {
        return false;
    }
    output = {input.value.data(), input.length};
    return valid_text(output);
}

/**
 * Reads one validated activity name.
 * @param input Source activity definition.
 * @param output View into `input`, or empty on failure.
 * @return True when the name is terminated and valid UTF-8.
 */
[[nodiscard]] bool
activity_name_view(const middleware::content::packages::tables::ActivityDefinition& input,
                   std::string_view& output) noexcept {
    output = {};
    if (input.internalNameLength >= input.internalName.size()
        || input.internalName[input.internalNameLength] != '\0') {
        return false;
    }
    output = {input.internalName.data(), input.internalNameLength};
    return valid_text(output);
}

[[nodiscard]] bool policy_text_view(const policy_inventory::Snapshot& source,
                                    policy_inventory::Text input,
                                    std::string_view& output) noexcept {
    output = {};
    if (input.stringIndex >= source.strings.size()) {
        return false;
    }
    output = source.strings[input.stringIndex];
    return valid_text(output);
}

template <typename Left, typename Right>
[[nodiscard]] bool same_text(const Left& left, const Right& right) noexcept {
    std::string_view leftView{};
    std::string_view rightView{};
    return text_view(left, leftView) && text_view(right, rightView) && leftView == rightView;
}

[[nodiscard]] bool same_range(format::Range left, format::Range right) noexcept {
    return left.first == right.first && left.count == right.count;
}

[[nodiscard]] bool range_inside(format::Range value, std::size_t count) noexcept {
    return value.first <= count && value.count <= count - value.first;
}

/**
 * Builds closed child ranges for rows grouped by owner.
 * @param rows Child rows in owner order.
 * @param ownerCount Number of possible owners.
 * @param owner Reads an owner index from one row.
 * @param output Complete range table, or partial data on failure.
 * @return True when every owner index and range is valid.
 */
template <typename Row, typename Owner>
[[nodiscard]] bool make_owner_ranges(std::span<const Row> rows,
                                     std::size_t ownerCount,
                                     Owner owner,
                                     std::vector<format::Range>& output) {
    if (ownerCount >= format::kAbsentIndex || rows.size() >= format::kAbsentIndex) {
        return false;
    }
    output.assign(ownerCount, {});
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const std::uint32_t ownerIndex = owner(rows[index]);
        if (ownerIndex >= ownerCount) {
            return false;
        }
        format::Range& range = output[ownerIndex];
        if (range.count == 0) {
            range.first = static_cast<std::uint32_t>(index);
            range.count = 1;
        } else if (range.first + range.count == index) {
            ++range.count;
        } else {
            return false;
        }
    }
    return true;
}

/**
 * Builds closed ranges for one policy subject kind.
 * @param policy Accepted policy inventory.
 * @param kind Subject kind to include.
 * @param ownerCount Number of possible subjects of `kind`.
 * @param output Complete range table, or partial data on failure.
 * @return True when every matching subject and range is valid.
 */
[[nodiscard]] bool make_capability_ranges(const policy_inventory::Snapshot& policy,
                                          format::SubjectKind kind,
                                          std::size_t ownerCount,
                                          std::vector<format::Range>& output) {
    if (ownerCount >= format::kAbsentIndex || policy.capabilities.size() >= format::kAbsentIndex) {
        return false;
    }
    output.assign(ownerCount, {});
    const std::uint32_t expectedKind = static_cast<std::uint32_t>(kind);
    for (std::size_t index = 0; index < policy.capabilities.size(); ++index) {
        const policy_inventory::Capability& row = policy.capabilities[index];
        if (row.subjectKind != expectedKind) {
            continue;
        }
        if (row.subjectIndex >= ownerCount) {
            return false;
        }
        format::Range& range = output[row.subjectIndex];
        if (range.count == 0) {
            range.first = static_cast<std::uint32_t>(index);
            range.count = 1;
        } else if (range.first + range.count == index) {
            ++range.count;
        } else {
            return false;
        }
    }
    return true;
}

/** One linker owns the two-tier string policy, so both tiers stay in step. */
class StringLinker final {
public:
    [[nodiscard]] bool keep_legacy(std::string_view value) {
        return keep(value, legacy_);
    }

    [[nodiscard]] bool keep_extension(std::string_view value) {
        return keep(value, extension_);
    }

    /** Builds the final byte bank and all temporary lookup references. */
    [[nodiscard]] bool finish(std::vector<std::byte>& output) {
        canonicalize(legacy_);
        canonicalize(extension_);
        extension_.erase(std::remove_if(extension_.begin(),
                                        extension_.end(),
                                        [this](std::string_view value) {
                                            return std::binary_search(
                                                legacy_.begin(), legacy_.end(), value, byte_less);
                                        }),
                         extension_.end());

        std::size_t total = 0;
        for (const std::string_view value : legacy_) {
            if (value.size() > (std::numeric_limits<std::uint32_t>::max)()
                || total > (std::numeric_limits<std::uint32_t>::max)() - value.size()) {
                return false;
            }
            total += value.size();
        }
        for (const std::string_view value : extension_) {
            if (value.size() > (std::numeric_limits<std::uint32_t>::max)()
                || total > (std::numeric_limits<std::uint32_t>::max)() - value.size()) {
                return false;
            }
            total += value.size();
        }

        output.resize(total);
        references_.clear();
        references_.reserve(legacy_.size() + extension_.size() + 1U);
        references_.emplace(std::string_view{}, format::StringRef{});
        std::size_t cursor = 0;
        for (const std::string_view value : legacy_) {
            append(value, cursor, output);
        }
        for (const std::string_view value : extension_) {
            append(value, cursor, output);
        }
        return cursor == output.size();
    }

    [[nodiscard]] bool reference(std::string_view value, format::StringRef& output) const noexcept {
        const auto found = references_.find(value);
        if (found == references_.end()) {
            output = {};
            return false;
        }
        output = found->second;
        return true;
    }

private:
    [[nodiscard]] static bool keep(std::string_view value, std::vector<std::string_view>& output) {
        if (!valid_text(value)) {
            return false;
        }
        if (!value.empty()) {
            output.push_back(value);
        }
        return true;
    }

    static void canonicalize(std::vector<std::string_view>& values) {
        std::sort(values.begin(), values.end(), byte_less);
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    void append(std::string_view value, std::size_t& cursor, std::vector<std::byte>& output) {
        const format::StringRef reference{static_cast<std::uint32_t>(cursor),
                                          static_cast<std::uint32_t>(value.size())};
        if (!value.empty()) {
            std::memcpy(output.data() + cursor, value.data(), value.size());
        }
        references_.emplace(value, reference);
        cursor += value.size();
    }

    std::vector<std::string_view> legacy_{};
    std::vector<std::string_view> extension_{};
    std::unordered_map<std::string_view, format::StringRef> references_{};
};

[[nodiscard]] bool
resolve_string(const void* context, std::string_view value, format::StringRef& output) noexcept {
    return static_cast<const StringLinker*>(context)->reference(value, output);
}

template <typename Text> [[nodiscard]] bool keep_legacy(StringLinker& linker, const Text& input) {
    std::string_view value{};
    return text_view(input, value) && linker.keep_legacy(value);
}

template <typename Text>
[[nodiscard]] bool keep_extension(StringLinker& linker, const Text& input) {
    std::string_view value{};
    return text_view(input, value) && linker.keep_extension(value);
}

template <typename Text>
[[nodiscard]] bool
link_text(const StringLinker& linker, const Text& input, format::StringRef& output) noexcept {
    std::string_view value{};
    return text_view(input, value) && linker.reference(value, output);
}

[[nodiscard]] bool link_string(const StringLinker& linker,
                               std::string_view input,
                               format::StringRef& output) noexcept {
    return valid_text(input) && linker.reference(input, output);
}

[[nodiscard]] bool link_policy_text(const StringLinker& linker,
                                    const policy_inventory::Snapshot& source,
                                    policy_inventory::Text input,
                                    format::StringRef& output) noexcept {
    std::string_view value{};
    return policy_text_view(source, input, value) && linker.reference(value, output);
}

[[nodiscard]] bool all_inputs_present(const Inputs& inputs) noexcept {
    return inputs.activityInventory != nullptr && inputs.activityEnrichment != nullptr
           && inputs.topology != nullptr && inputs.topologyEnrichment != nullptr
           && inputs.policy != nullptr && inputs.actorRsat != nullptr
           && inputs.squadFacts != nullptr && inputs.squads != nullptr
           && inputs.authoredScenes != nullptr && inputs.behaviors != nullptr;
}

/** Checks accepted-snapshot markers, fixed counts, and cross-inventory parent identity. */
[[nodiscard]] bool prepare_inputs(const Inputs& inputs, PreparedRanges& ranges) {
    if (!all_inputs_present(inputs)) {
        return false;
    }
    const activity_inventory::Snapshot& activityInventory = *inputs.activityInventory;
    const activity_enrichment_inventory::Snapshot& activityEnrichment = *inputs.activityEnrichment;
    const topology_inventory::Snapshot& topology = *inputs.topology;
    const topology_enrichment::Snapshot& topologyEnrichment = *inputs.topologyEnrichment;
    const policy_inventory::Snapshot& policy = *inputs.policy;
    const actor_rsat_inventory::Snapshot& actorRsat = *inputs.actorRsat;
    const squad_inventory::Facts& squadFacts = *inputs.squadFacts;
    const squad_inventory::Snapshot& squads = *inputs.squads;
    const authored_scene_inventory::Snapshot& authoredScenes = *inputs.authoredScenes;
    const behavior_inventory::Snapshot& behaviors = *inputs.behaviors;

    const bool baseReady =
        topology.ready && !topology.nameInventoryComplete && !topology.stringInventoryComplete
        && topology.nextScenario == topology.scenarios.size() && actorRsat.complete
        && squadFacts.complete && squadFacts.unresolvedReads == 0 && squads.ready
        && squads.actorLinksComplete && authoredScenes.complete && behaviors.ready;
    if (!baseReady || !activity_inventory::validate(activityInventory)
        || !activity_enrichment_inventory::validate(activityEnrichment)
        || activityInventory.activities.size() != topology.activities.size()
        || activityInventory.scenarios.size() != topology.scenarios.size()
        || activityEnrichment.rows.size() != topology.activities.size()
        || topologyEnrichment.bubbleNames.size() != topology.bubbles.size()
        || topologyEnrichment.slots.size() != topology.slots.size()
        || policy.activityAliases.size() != topology.activities.size()
        || policy.activityCapabilities.size() != topology.activities.size()
        || policy.slotAliases.size() != topology.slots.size()
        || policy.slotCapabilities.size() != topology.slots.size()) {
        return false;
    }

    for (std::size_t index = 0; index < topology.scenarios.size(); ++index) {
        const activity_inventory::ScenarioRoot& source = activityInventory.scenarios[index];
        const topology_inventory::Scenario& target = topology.scenarios[index];
        std::string_view sourceName{};
        std::string_view targetName{};
        if (source.nameLength >= source.name.size() || source.name[source.nameLength] != '\0'
            || !valid_text(sourceName = {source.name.data(), source.nameLength})
            || !text_view(target.name, targetName) || source.tag != target.tag
            || sourceName != targetName) {
            return false;
        }
    }

    for (std::size_t index = 0; index < topology.activities.size(); ++index) {
        const activity_inventory::ActivityVariant& source = activityInventory.activities[index];
        const activity_enrichment_inventory::Row& enrichment = activityEnrichment.rows[index];
        const topology_inventory::Activity& target = topology.activities[index];
        std::string_view sourceName{};
        std::string_view targetName{};
        if (!activity_name_view(source.definition, sourceName)
            || !text_view(target.internalName, targetName)
            || source.definition.activityIndex != target.activityIndex
            || source.definition.definitionHash != target.definitionHash
            || enrichment.activityIndex != target.activityIndex
            || enrichment.definitionHash != target.definitionHash || sourceName != targetName
            || !same_text(enrichment.displayName, target.displayName)) {
            return false;
        }
        const bool joined = source.joinStatus == activity_inventory::JoinStatus::exact;
        if (joined) {
            if (target.scenarioIndex >= topology.scenarios.size()
                || topology.scenarios[target.scenarioIndex].tag != source.scenarioTag
                || target.exactFlags != format::kActivityExactMask) {
                return false;
            }
        } else if (target.scenarioIndex != format::kAbsentIndex || target.exactFlags != 0) {
            return false;
        }
    }

    std::uint32_t aliasCursor = 0;
    for (const topology_enrichment::Slot& row : topologyEnrichment.slots) {
        if (row.aliases.first != aliasCursor
            || !range_inside(row.aliases, topologyEnrichment.slotAliases.size())) {
            return false;
        }
        aliasCursor += row.aliases.count;
    }
    if (aliasCursor != topologyEnrichment.slotAliases.size()) {
        return false;
    }
    for (const topology_inventory::Text& value : topologyEnrichment.bubbleNames) {
        std::string_view ignored{};
        if (!text_view(value, ignored)) {
            return false;
        }
    }
    for (const topology_inventory::Text& value : topologyEnrichment.slotAliases) {
        std::string_view alias{};
        if (!text_view(value, alias) || alias.empty()) {
            return false;
        }
    }

    if (policy.strings.empty() || !policy.strings.front().empty()
        || policy.strings.size() >= format::kAbsentIndex) {
        return false;
    }
    std::unordered_set<std::string_view> policyStrings{};
    policyStrings.reserve(policy.strings.size());
    for (std::size_t index = 0; index < policy.strings.size(); ++index) {
        const std::string_view value = policy.strings[index];
        if (!valid_text(value) || (index != 0 && value.empty())
            || !policyStrings.insert(value).second) {
            return false;
        }
    }

    if (!make_owner_ranges(
            std::span<const topology_inventory::Bubble>(topology.bubbles),
            topology.scenarios.size(),
            [](const topology_inventory::Bubble& row) { return row.scenarioIndex; },
            ranges.scenarioBubbles)
        || !make_owner_ranges(
            std::span<const topology_inventory::State>(topology.states),
            topology.scenarios.size(),
            [](const topology_inventory::State& row) { return row.scenarioIndex; },
            ranges.scenarioStates)
        || !make_owner_ranges(
            std::span<const topology_inventory::Occurrence>(topology.occurrences),
            topology.scenarios.size(),
            [](const topology_inventory::Occurrence& row) { return row.scenarioIndex; },
            ranges.scenarioOccurrences)
        || !make_owner_ranges(
            std::span<const topology_inventory::State>(topology.states),
            topology.bubbles.size(),
            [](const topology_inventory::State& row) { return row.bubbleIndex; },
            ranges.bubbleStates)
        || !make_owner_ranges(
            std::span<const topology_inventory::Slot>(topology.slots),
            topology.objects.size(),
            [](const topology_inventory::Slot& row) { return row.objectIndex; },
            ranges.objectSlots)
        || !make_capability_ranges(policy,
                                   format::SubjectKind::activity,
                                   topology.activities.size(),
                                   ranges.activityCapabilities)
        || !make_capability_ranges(
            policy, format::SubjectKind::slot, topology.slots.size(), ranges.slotCapabilities)
        || !make_capability_ranges(policy,
                                   format::SubjectKind::hostApi,
                                   policy.hostSubjects.size(),
                                   ranges.hostCapabilities)) {
        return false;
    }

    for (std::size_t index = 0; index < ranges.activityCapabilities.size(); ++index) {
        if (ranges.activityCapabilities[index].count == 0
            || !same_range(ranges.activityCapabilities[index],
                           policy.activityCapabilities[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < ranges.slotCapabilities.size(); ++index) {
        if (ranges.slotCapabilities[index].count == 0
            || !same_range(ranges.slotCapabilities[index], policy.slotCapabilities[index])) {
            return false;
        }
    }
    if (ranges.hostCapabilities.size() != policy.hostSubjects.size()) {
        return false;
    }
    std::string_view priorHost{};
    for (std::size_t index = 0; index < policy.hostSubjects.size(); ++index) {
        std::string_view host{};
        if (!policy_text_view(policy, policy.hostSubjects[index].id, host) || host.empty()
            || ranges.hostCapabilities[index].count == 0
            || !same_range(ranges.hostCapabilities[index], policy.hostSubjects[index].capabilities)
            || (index != 0 && !byte_less(priorHost, host))) {
            return false;
        }
        priorHost = host;
    }
    return true;
}

/** Derives only the parent ranges required to translate generation-owned rows. */
[[nodiscard]] bool prepare_generated_ranges(const Inputs& inputs, PreparedRanges& ranges) {
    if (!all_inputs_present(inputs)) {
        return false;
    }
    const topology_inventory::Snapshot& topology = *inputs.topology;
    const policy_inventory::Snapshot& policy = *inputs.policy;
    return make_owner_ranges(
               std::span<const topology_inventory::Bubble>(topology.bubbles),
               topology.scenarios.size(),
               [](const topology_inventory::Bubble& row) { return row.scenarioIndex; },
               ranges.scenarioBubbles)
           && make_owner_ranges(
               std::span<const topology_inventory::State>(topology.states),
               topology.scenarios.size(),
               [](const topology_inventory::State& row) { return row.scenarioIndex; },
               ranges.scenarioStates)
           && make_owner_ranges(
               std::span<const topology_inventory::Occurrence>(topology.occurrences),
               topology.scenarios.size(),
               [](const topology_inventory::Occurrence& row) { return row.scenarioIndex; },
               ranges.scenarioOccurrences)
           && make_owner_ranges(
               std::span<const topology_inventory::State>(topology.states),
               topology.bubbles.size(),
               [](const topology_inventory::State& row) { return row.bubbleIndex; },
               ranges.bubbleStates)
           && make_owner_ranges(
               std::span<const topology_inventory::Slot>(topology.slots),
               topology.objects.size(),
               [](const topology_inventory::Slot& row) { return row.objectIndex; },
               ranges.objectSlots)
           && make_capability_ranges(policy,
                                     format::SubjectKind::activity,
                                     topology.activities.size(),
                                     ranges.activityCapabilities)
           && make_capability_ranges(
               policy, format::SubjectKind::slot, topology.slots.size(), ranges.slotCapabilities)
           && make_capability_ranges(policy,
                                     format::SubjectKind::hostApi,
                                     policy.hostSubjects.size(),
                                     ranges.hostCapabilities);
}

/** Collects only strings referenced by legacy sections zero through twenty-two. */
[[nodiscard]] bool collect_legacy_strings(const Inputs& inputs, StringLinker& linker) {
    const topology_inventory::Snapshot& topology = *inputs.topology;
    const topology_enrichment::Snapshot& enrichment = *inputs.topologyEnrichment;
    const policy_inventory::Snapshot& policy = *inputs.policy;
    const actor_rsat_inventory::Snapshot& actorRsat = *inputs.actorRsat;
    const squad_inventory::Snapshot& squads = *inputs.squads;
    const authored_scene_inventory::Snapshot& authoredScenes = *inputs.authoredScenes;

    for (const topology_inventory::Activity& row : topology.activities) {
        if (!keep_legacy(linker, row.id) || !keep_legacy(linker, row.internalName)
            || !keep_legacy(linker, row.displayName)) {
            return false;
        }
    }
    for (const topology_inventory::Scenario& row : topology.scenarios) {
        if (!keep_legacy(linker, row.id) || !keep_legacy(linker, row.name)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < topology.bubbles.size(); ++index) {
        if (!keep_legacy(linker, topology.bubbles[index].id)
            || !keep_legacy(linker, enrichment.bubbleNames[index])) {
            return false;
        }
    }
    for (const topology_inventory::State& row : topology.states) {
        if (!keep_legacy(linker, row.id) || !keep_legacy(linker, row.entryId)
            || !keep_legacy(linker, row.registryId)) {
            return false;
        }
    }
    for (const topology_inventory::Object& row : topology.objects) {
        if (!keep_legacy(linker, row.id)) {
            return false;
        }
    }
    for (const topology_inventory::Occurrence& row : topology.occurrences) {
        if (!keep_legacy(linker, row.id) || !keep_legacy(linker, row.contextRegistryKey)
            || !keep_legacy(linker, row.registryId) || !keep_legacy(linker, row.entryId)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < topology.slots.size(); ++index) {
        const topology_enrichment::Slot& linked = enrichment.slots[index];
        if (!keep_legacy(linker, topology.slots[index].id) || !keep_legacy(linker, linked.name)
            || !keep_legacy(linker, linked.senseSchemaId)
            || !keep_legacy(linker, linked.authSchemaId)) {
            return false;
        }
    }

    const auto keepPolicy = [&policy, &linker](policy_inventory::Text text) {
        std::string_view value{};
        return policy_text_view(policy, text, value) && linker.keep_legacy(value);
    };
    for (const policy_inventory::TextRow& row : policy.texts) {
        if (!keepPolicy(row.value)) {
            return false;
        }
    }
    for (const policy_inventory::Capability& row : policy.capabilities) {
        if (!keepPolicy(row.id) || !keepPolicy(row.operation) || !keepPolicy(row.valueSchemaId)) {
            return false;
        }
    }
    for (const policy_inventory::Gate& row : policy.gates) {
        if (!keepPolicy(row.gate) || !keepPolicy(row.status) || !keepPolicy(row.reasonCode)
            || !keepPolicy(row.required) || !keepPolicy(row.observed)
            || !keepPolicy(row.wouldConfirm)) {
            return false;
        }
    }
    for (const policy_inventory::Refusal& row : policy.refusals) {
        if (!keepPolicy(row.id) || !keepPolicy(row.exposure) || !keepPolicy(row.status)) {
            return false;
        }
    }

    for (const auto& entry : actorRsat.sequenceEntries) {
        if (!linker.keep_extension(entry.id) || !linker.keep_extension(entry.name)
            || !linker.keep_extension(entry.symbol) || !linker.keep_extension(entry.sourcePath)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::ActorClass& row : actorRsat.actorClasses) {
        if (!keep_legacy(linker, row.id)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::ActorMessageSchema& row : actorRsat.messageSchemas) {
        if (!keep_legacy(linker, row.name)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::ActorCommandDefinition& row : actorRsat.commandDefinitions) {
        if (!keep_legacy(linker, row.name) || !keep_legacy(linker, row.factionNoneName)
            || !keep_legacy(linker, row.factionRemovedName)
            || !keep_legacy(linker, row.factionHostileToAllName)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::SimulationEventDefinition& row : actorRsat.simulationEvents) {
        if (!keep_legacy(linker, row.name)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::RuntimeTypeDefinition& row : actorRsat.runtimeTypes) {
        if (!keep_legacy(linker, row.name)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::EntityTypeDefinition& row : actorRsat.entityTypes) {
        if (!keep_legacy(linker, row.name)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::RsatDescriptor& row : actorRsat.descriptors) {
        if (!keep_legacy(linker, row.id)) {
            return false;
        }
    }
    for (const actor_rsat_inventory::RsatSchema& row : actorRsat.schemas) {
        if (!keep_legacy(linker, row.id)) {
            return false;
        }
    }
    for (const squad_inventory::Squad& row : squads.squads) {
        if (!linker.keep_legacy(row.id)) {
            return false;
        }
    }
    for (const squad_inventory::SquadMember& row : squads.members) {
        if (!linker.keep_legacy(row.id)) {
            return false;
        }
    }
    for (const squad_inventory::SquadAnchor& row : squads.anchors) {
        if (!linker.keep_legacy(row.id)) {
            return false;
        }
    }
    for (const authored_scene_inventory::Resource& row : authoredScenes.resources) {
        if (!keep_legacy(linker, row.id)) {
            return false;
        }
    }
    for (const authored_scene_inventory::SquadEdge& row : authoredScenes.squadEdges) {
        if (!keep_legacy(linker, row.id)) {
            return false;
        }
    }
    for (const authored_scene_inventory::TaskTarget& row : authoredScenes.taskTargets) {
        if (!keep_legacy(linker, row.id)) {
            return false;
        }
    }
    for (const authored_scene_inventory::DialogueCueText& row : authoredScenes.dialogueCueTexts) {
        if (!keep_legacy(linker, row.id) || !keep_legacy(linker, row.text)) {
            return false;
        }
    }
    for (const authored_scene_inventory::DirectiveElement& row : authoredScenes.directiveElements) {
        if (!keep_legacy(linker, row.id) || !keep_legacy(linker, row.title)
            || !keep_legacy(linker, row.description)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** @return Borrowed sections in exact current format order. */
pack::Tables Storage::tables() const noexcept {
    return {
        strings,
        activities,
        scenarios,
        bubbles,
        states,
        objects,
        occurrences,
        slots,
        texts,
        capabilities,
        gates,
        refusals,
        actorClasses,
        rsatDescriptors,
        rsatSchemas,
        rsatFields,
        squads,
        squadMembers,
        squadAnchors,
        authoredSceneResources,
        authoredSceneSquadEdges,
        taskTargets,
        dialogueCueTexts,
        directiveElements,
        activityBindingTags,
        activityBindingLocators,
        behaviorPrograms,
        behaviorInputs,
        behaviorChannelWrites,
        behaviorOwners,
        behaviorActivityBindings,
        actorMessageSchemas,
        actorCommandDefinitions,
        actorBehaviorProfiles,
        simulationEventDefinitions,
        runtimeSchemas,
        runtimeFields,
        sobjectRsats,
        sobjectRsatDescriptors,
        entityTypeDefinitions,
        sobjectRsatFieldBindings,
        runtimeTypeDefinitions,
        actorStateNames,
        actorSequenceTables,
        actorSequenceEntries,
        actorSequenceBindings,
    };
}

/**
 * Links accepted native inventories without changing `output` on failure.
 * @param inputs Complete accepted inventories.
 * @param output Owned final rows.
 * @return True when every row and reference closes.
 */
bool compose(const Inputs& inputs, Storage& output) noexcept {
    try {
        PreparedRanges ranges{};
        if (!prepare_inputs(inputs, ranges)) {
            return false;
        }
        StringLinker linker{};
        Storage pending{};
        const detail::StringResolver strings{&linker, resolve_string};
        if (!collect_legacy_strings(inputs, linker)) {
            return false;
        }
        if (!linker.finish(pending.strings)) {
            return false;
        }
        if (!detail::translate_rows(inputs, ranges, strings, pending)) {
            return false;
        }
        if (!detail::validate_storage(inputs, ranges, pending)) {
            return false;
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        return false;
    }
}

/** Links generation-owned inventories without running the validation pass. */
bool compose_generated(const Inputs& inputs, Storage& output) noexcept {
    try {
        PreparedRanges ranges{};
        if (!prepare_generated_ranges(inputs, ranges)) {
            return false;
        }
        StringLinker linker{};
        Storage pending{};
        const detail::StringResolver strings{&linker, resolve_string};
        if (!collect_legacy_strings(inputs, linker) || !linker.finish(pending.strings)
            || !detail::translate_rows(inputs, ranges, strings, pending)) {
            return false;
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * Builds a format-v12 image without changing outputs on failure.
 * @param identity Required pack identity.
 * @param inputs Complete accepted inventories.
 * @param tables Owned final rows.
 * @param image Serialized pack image.
 * @param payloadSha256 Digest of the serialized payload.
 * @return Writer status, or an input or canonical-digest failure.
 */
pack::Status build(const pack::Identity& identity,
                   const Inputs& inputs,
                   Storage& tables,
                   std::vector<std::byte>& image,
                   pack::Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    Storage pendingTables{};
    if (!compose(inputs, pendingTables)) {
        return pack::Status::invalidInput;
    }
    std::vector<std::byte> pendingImage{};
    pack::Digest pendingDigest{};
    const pack::Status status =
        pack::build(identity, pendingTables.tables(), pendingImage, pendingDigest);
    if (status != pack::Status::ready) {
        return status;
    }
    tables = std::move(pendingTables);
    image = std::move(pendingImage);
    payloadSha256 = pendingDigest;
    return pack::Status::ready;
}

} // namespace sunrise::client::content::activity::sdk_generation::pack_composer
