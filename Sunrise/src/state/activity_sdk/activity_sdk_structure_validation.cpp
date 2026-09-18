#include <array>

#include "validation_internal.h"

namespace sunrise::state::activity_sdk::validation {
namespace {

/** @return True when one row range stays inside a section of `size` rows. */
[[nodiscard]] constexpr bool range(format::Range value, std::size_t size) noexcept {
    return value.first <= size && value.count <= size - value.first;
}

template <typename Row, typename Id>
[[nodiscard]] bool required_ids(std::span<const Row> rows, const Catalog& catalog, Id id) noexcept {
    for (const Row& row : rows) {
        const std::string_view value = catalog.string(id(row));
        if (value.empty() || value.find('\0') != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

/** @return True when every directive element has a valid description or progress label. */
[[nodiscard]] bool directive_texts(const Catalog& catalog) noexcept {
    for (const format::DirectiveElement& row : catalog.directive_elements()) {
        const std::string_view description = catalog.string(row.description);
        const std::string_view progress = catalog.string(row.progress);
        if ((description.empty() && progress.empty())
            || description.find('\0') != std::string_view::npos
            || progress.find('\0') != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

/** @return True when every activity's row ranges stay inside the sections they index. */
[[nodiscard]] bool activity_ranges(const Catalog& catalog) noexcept {
    for (const format::Activity& row : catalog.activities()) {
        if (!range(row.aliases, catalog.texts().size())
            || !range(row.capabilities, catalog.capabilities().size())
            || !range(row.activityRootCandidateTags, catalog.activity_binding_tags().size())
            || !range(row.scenarioNameCandidateTags, catalog.activity_binding_tags().size())
            || !range(row.evidenceRootTags, catalog.activity_binding_tags().size())
            || !range(row.bindingLocators, catalog.activity_binding_locators().size())) {
            return false;
        }
    }
    return true;
}

/** @return True when every topology row range stays inside the sections it indexes. */
[[nodiscard]] bool topology_ranges(const Catalog& catalog) noexcept {
    for (const format::Scenario& row : catalog.scenarios()) {
        if (!range(row.bubbles, catalog.bubbles().size())
            || !range(row.states, catalog.states().size())
            || !range(row.occurrences, catalog.occurrences().size())) {
            return false;
        }
    }
    for (const format::Object& row : catalog.objects()) {
        if (!range(row.slots, catalog.slots().size())) {
            return false;
        }
    }
    for (const format::Squad& row : catalog.squads()) {
        if (!range(row.members, catalog.squad_members().size())
            || !range(row.anchors, catalog.squad_anchors().size())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool behavior_ranges(const Catalog& catalog) noexcept {
    for (const format::BehaviorProgram& row : catalog.behavior_programs()) {
        if (!range(row.inputs, catalog.behavior_inputs().size())
            || !range(row.channelWrites, catalog.behavior_channel_writes().size())) {
            return false;
        }
    }
    return true;
}

/** @return True when every runtime row range stays inside the sections it indexes. */
[[nodiscard]] bool runtime_ranges(const Catalog& catalog) noexcept {
    for (const format::RuntimeSchema& row : catalog.runtime_schemas()) {
        if (!range(row.fields, catalog.runtime_fields().size())) {
            return false;
        }
    }
    for (const format::SobjectRsat& row : catalog.sobject_rsats()) {
        if (!range(row.descriptors, catalog.sobject_rsat_descriptors().size())) {
            return false;
        }
    }
    return true;
}

/** @return True when every actor command carries a resolvable, unique name. */
[[nodiscard]] bool actor_command_names(const Catalog& catalog) noexcept {
    for (const format::ActorCommandDefinition& row : catalog.actor_command_definitions()) {
        const bool setFaction = row.effect == format::ActorCommandEffect::setFaction;
        const std::array factionNames{
            row.factionNoneName, row.factionRemovedName, row.factionHostileToAllName};
        for (const format::StringRef reference : factionNames) {
            const std::string_view value = catalog.string(reference);
            if ((setFaction && (value.empty() || value.find('\0') != std::string_view::npos))
                || (!setFaction && (reference.offset != 0 || reference.length != 0))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/** Checks that every section is non-empty, uniquely keyed, and indexed inside its bounds. */
bool structure(const Catalog& catalog) {
    return !catalog.activities().empty() && !catalog.scenarios().empty()
           && required_ids(catalog.activities(), catalog, [](const auto& row) { return row.id; })
           && required_ids(catalog.scenarios(), catalog, [](const auto& row) { return row.id; })
           && required_ids(catalog.objects(), catalog, [](const auto& row) { return row.id; })
           && required_ids(catalog.slots(), catalog, [](const auto& row) { return row.id; })
           && required_ids(catalog.squads(), catalog, [](const auto& row) { return row.id; })
           && required_ids(
               catalog.actor_message_schemas(), catalog, [](const auto& row) { return row.name; })
           && required_ids(catalog.actor_command_definitions(),
                           catalog,
                           [](const auto& row) { return row.name; })
           && actor_command_names(catalog)
           && required_ids(catalog.simulation_event_definitions(),
                           catalog,
                           [](const auto& row) { return row.name; })
           && required_ids(
               catalog.entity_type_definitions(), catalog, [](const auto& row) { return row.name; })
           && required_ids(catalog.task_targets(), catalog, [](const auto& row) { return row.id; })
           && required_ids(
               catalog.dialogue_cue_texts(), catalog, [](const auto& row) { return row.id; })
           && required_ids(
               catalog.dialogue_cue_texts(), catalog, [](const auto& row) { return row.text; })
           && required_ids(
               catalog.directive_elements(), catalog, [](const auto& row) { return row.id; })
           && required_ids(
               catalog.directive_elements(), catalog, [](const auto& row) { return row.title; })
           && directive_texts(catalog) && activity_ranges(catalog) && topology_ranges(catalog)
           && behavior_ranges(catalog) && runtime_ranges(catalog);
}

} // namespace sunrise::state::activity_sdk::validation
