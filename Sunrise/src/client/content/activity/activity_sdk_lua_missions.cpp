#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../middleware/bap/activity_message/auth_schema_catalog.h"
#include "activity_sdk_lua_missions_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

namespace auth_catalog = middleware::bap::activity_message::auth_schema_catalog;

void append_int(std::string& output, std::int32_t value) {
    std::array<char, 16> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "%d", value);
    output.append(buffer.data(), static_cast<std::size_t>(length));
}

void append_float(std::string& output, std::uint32_t bits) {
    std::array<char, 32> buffer{};
    const int length = std::snprintf(
        buffer.data(), buffer.size(), "%.9g", static_cast<double>(std::bit_cast<float>(bits)));
    output.append(buffer.data(), static_cast<std::size_t>(length));
}

/** Builds the exact occurrence-bound identity consumed by the mission scene API. */
[[nodiscard]] bool scene_symbol_id(const Source& source,
                                   const format::Occurrence& occurrence,
                                   const format::Slot& slot,
                                   std::string& output) {
    // Occurrence ids carry this prefix; the scene symbol keeps the remainder.
    constexpr std::string_view kOccurrencePrefix = "object-occurrence/";
    const std::string_view occurrenceId = text(source, occurrence.id);
    const std::string_view suffix = occurrenceId.starts_with(kOccurrencePrefix)
                                        ? occurrenceId.substr(kOccurrencePrefix.size())
                                        : occurrenceId;
    if (suffix.empty() || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()
        || slot.slotType > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    std::array<char, 32> tail{};
    const int length =
        std::snprintf(tail.data(), tail.size(), "/%04x/%04x", slot.slotIndex, slot.slotType);
    if (length <= 0 || static_cast<std::size_t>(length) >= tail.size()) {
        return false;
    }
    output.assign("symbol/");
    output.append(suffix);
    output.append(tail.data(), static_cast<std::size_t>(length));
    return true;
}

/** Collects each global slot row used by one scenario exactly once. */
[[nodiscard]] bool scenario_slots(const Source& source,
                                  const format::Scenario& scenario,
                                  std::vector<std::uint32_t>& output) {
    output.clear();
    if (!range_inside(scenario.occurrences, source.occurrences.size())) {
        return false;
    }
    for (std::size_t offset = 0; offset < scenario.occurrences.count; ++offset) {
        const format::Occurrence& occurrence =
            source.occurrences[scenario.occurrences.first + offset];
        if (occurrence.objectIndex >= source.objects.size()) {
            return false;
        }
        const format::Object& object = source.objects[occurrence.objectIndex];
        if (!range_inside(object.slots, source.slots.size())) {
            return false;
        }
        for (std::size_t slotOffset = 0; slotOffset < object.slots.count; ++slotOffset) {
            output.push_back(object.slots.first + static_cast<std::uint32_t>(slotOffset));
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return true;
}

/** Folds slot names into keys. A repeated name takes its object tag, then its slot index. */
[[nodiscard]] std::vector<std::string> unique_slot_keys(const Source& source,
                                                        const std::vector<std::uint32_t>& rows,
                                                        const std::vector<std::string>& names) {
    std::vector<std::string> keys(rows.size());
    std::unordered_map<std::string, std::size_t> counts;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        keys[index] = identifier(names[index], true);
        ++counts[keys[index]];
    }
    // Every member of a repeated group is suffixed, so a key never depends on row order.
    std::unordered_map<std::string, std::size_t> taggedCounts;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (counts[keys[index]] > 1) {
            const format::Slot& slot = source.slots[rows[index]];
            const std::uint32_t tag = slot.objectIndex < source.objects.size()
                                          ? source.objects[slot.objectIndex].objectTag
                                          : 0U;
            std::array<char, 16> suffix{};
            const int length = std::snprintf(suffix.data(), suffix.size(), "_%08X", tag);
            keys[index].append(suffix.data(), static_cast<std::size_t>(length));
        }
        ++taggedCounts[keys[index]];
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (taggedCounts[keys[index]] > 1) {
            std::array<char, 16> suffix{};
            const int length =
                std::snprintf(suffix.data(),
                              suffix.size(),
                              "_%04X",
                              static_cast<unsigned>(source.slots[rows[index]].slotIndex));
            keys[index].append(suffix.data(), static_cast<std::size_t>(length));
        }
    }
    return keys;
}

} // namespace

/** Emits one concrete mission module with no pack or resolver access. */
bool render_mission(const Source& source,
                    const RenderIndex& index,
                    std::uint32_t scenarioIndex,
                    const format::Scenario& scenario,
                    SourceModule& module) {
    const std::string_view name = text(source, scenario.name);
    module.stem = stem(name, scenario.tag);
    std::string output = "-- Generated from installed mission data. Do not edit.\n"
                         "local sdk = require(\"sunrise.activity_sdk\")\n\n"
                         "---@type SunriseMission\n"
                         "local mission = {\n    name = ";
    append_string(output, name);
    output.append(",\n    id = ");
    append_string(output, text(source, scenario.id));
    output.append(",\n    tag = ");
    append_hex(output, scenario.tag);
    output.append(",\n    states = {\n");
    std::unordered_set<std::string> stateKeys;
    std::string stateConstants = "mission.State = {\n";
    if (!range_inside(scenario.states, source.states.size())) {
        return false;
    }
    for (std::size_t offset = 0; offset < scenario.states.count; ++offset) {
        const format::State& state = source.states[scenario.states.first + offset];
        const std::string_view id = text(source, state.id);
        const std::string key = append_unique_key(output, stateKeys, id, state.stateHash);
        output.append("{ id = ");
        append_string(output, id);
        output.append(", ordinal = ");
        append_uint(output, state.stateOrdinal);
        output.append(", slice_set_index = ");
        append_uint(output, state.sliceSetIndex);
        output.append(", region_index = ");
        append_uint(output, state.sliceSetIndex + state.stateOrdinal);
        output.append(", hash = ");
        append_hex(output, state.stateHash);
        output.append(", value = ");
        append_uint(output, state.publicValue);
        output.append(" },\n");
        stateConstants.append("    ");
        stateConstants.append(key);
        stateConstants.append(" = ");
        append_uint(stateConstants, state.publicValue);
        stateConstants.append(",\n");
    }
    output.append("    },\n    slots = {\n");
    std::unordered_map<std::uint32_t, std::string> slotKeyByRow;
    std::string slotConstants = "mission.Slot = {\n";
    std::array<std::string, auth_catalog::kTypes.size()> authConstants{};
    std::vector<std::uint32_t> slots;
    if (!scenario_slots(source, scenario, slots)) {
        return false;
    }
    std::vector<std::string> slotNames;
    slotNames.reserve(slots.size());
    for (const std::uint32_t row : slots) {
        const format::Slot& slot = source.slots[row];
        const auth_catalog::Type* const auth =
            auth_catalog::find(static_cast<std::uint8_t>(slot.slotType), slot.authSchema);
        std::string nameValue(text(source, slot.name));
        if (nameValue.empty()) {
            std::array<char, 24> suffix{};
            const int length = std::snprintf(
                suffix.data(), suffix.size(), "_%04X", static_cast<unsigned>(slot.slotIndex));
            nameValue.assign(auth == nullptr ? "slot" : auth->name);
            nameValue.append(suffix.data(), static_cast<std::size_t>(length));
        }
        slotNames.push_back(std::move(nameValue));
    }
    const std::vector<std::string> slotKeys = unique_slot_keys(source, slots, slotNames);
    slotKeyByRow.reserve(slots.size());
    for (std::size_t slotOrdinal = 0; slotOrdinal < slots.size(); ++slotOrdinal) {
        const std::uint32_t row = slots[slotOrdinal];
        const format::Slot& slot = source.slots[row];
        const auth_catalog::Type* const auth =
            auth_catalog::find(static_cast<std::uint8_t>(slot.slotType), slot.authSchema);
        const std::string_view nameValue = slotNames[slotOrdinal];
        const std::string& key = slotKeys[slotOrdinal];
        slotKeyByRow.emplace(row, key);
        output.append("    ");
        output.append(key);
        output.append(" = ");
        output.append("{ id = ");
        append_string(output, text(source, slot.id));
        output.append(", name = ");
        append_string(output, nameValue);
        output.append(", index = ");
        append_uint(output, slot.slotIndex);
        output.append(", type = ");
        append_uint(output, slot.slotType);
        output.append(", component_class = ");
        append_hex(output, slot.componentClass);
        output.append(", sense_schema = ");
        append_uint(output, slot.senseSchema);
        output.append(", auth_schema = ");
        append_uint(output, slot.authSchema);
        if ((slot.flags & format::kSlotAuthoredSceneUnresourced) != 0) {
            output.append(", unresourced = true");
        }
        if (auth != nullptr) {
            output.append(", auth_type = ");
            append_string(output, auth->name);
            output.append(", auth_min_bits = ");
            append_uint(output, auth->minimumBits);
            output.append(", auth_max_bits = ");
            append_uint(output, auth->maximumBits);
            output.append(", auth_dynamic = ");
            output.append(auth->hasDynamicBody ? "true" : "false");
            output.append(", auth_writable = ");
            output.append(auth->writable ? "true" : "false");
            if (auth->hasContiguousMirror) {
                output.append(", auth_component_offset = ");
                append_uint(output, auth->componentOffset);
            }
        }
        output.append(" },\n");
        slotConstants.append("    ");
        slotConstants.append(key);
        slotConstants.append(" = ");
        append_string(slotConstants, text(source, slot.id));
        slotConstants.append(",\n");
        if (auth != nullptr) {
            const std::size_t authRow =
                static_cast<std::size_t>(auth - auth_catalog::kTypes.data());
            authConstants[authRow].append("        ");
            authConstants[authRow].append(key);
            authConstants[authRow].append(" = mission.Slot.");
            authConstants[authRow].append(key);
            authConstants[authRow].append(",\n");
        }
    }
    output.append("    },\n    squads = {\n");
    std::unordered_set<std::string> squadKeys;
    std::string squadConstants = "mission.Squad = {\n";
    if (scenarioIndex >= index.squadsByScenario.size()) {
        return false;
    }
    for (const std::uint32_t squadRow : index.squadsByScenario[scenarioIndex]) {
        const format::Squad& squad = source.squads[squadRow];
        const std::string_view id = text(source, squad.id);
        const auto slotKey = slotKeyByRow.find(squad.slotIndex);
        const std::string_view squadName =
            slotKey == slotKeyByRow.end() ? id : std::string_view{slotKey->second};
        const std::string key = append_unique_key(output, squadKeys, squadName, squad.slotIndex);
        output.append("{ id = ");
        append_string(output, id);
        output.append(", slot = ");
        append_uint(output, squad.slotIndex);
        output.append(", spawner_config = ");
        append_hex(output, squad.spawnerConfigTag);
        output.append(", spawn_rule_config = ");
        append_hex(output, squad.spawnRuleConfigTag);
        output.append(", members = {\n");
        if (!range_inside(squad.members, source.squadMembers.size())) {
            return false;
        }
        for (std::size_t memberOffset = 0; memberOffset < squad.members.count; ++memberOffset) {
            const format::SquadMember& member =
                source.squadMembers[squad.members.first + memberOffset];
            const bool exactActor = (member.flags & format::kSquadMemberActorClassExact) != 0;
            if (!exactActor) {
                output.append("            { actor_class = nil, behavior_config = nil, "
                              "default_faction = nil },\n");
                continue;
            }
            if (member.actorClassIndex == format::kAbsentIndex
                || member.actorClassIndex >= source.actorClasses.size()
                || member.actorClassIndex >= source.actorBehaviorProfiles.size()) {
                return false;
            }
            const format::ActorClass& actor = source.actorClasses[member.actorClassIndex];
            const format::ActorBehaviorProfile& profile =
                source.actorBehaviorProfiles[member.actorClassIndex];
            if (profile.actorClassIndex != member.actorClassIndex) {
                return false;
            }
            output.append("            { actor_class = ");
            append_hex(output, actor.definitionTag);
            output.append(", behavior_config = ");
            append_hex(output, profile.behaviorConfigTag);
            output.append(", default_faction = ");
            append_int(output, profile.defaultFaction);
            output.append(" },\n");
        }
        output.append("        }, anchors = {\n");
        if (!range_inside(squad.anchors, source.squadAnchors.size())) {
            return false;
        }
        for (std::size_t anchorOffset = 0; anchorOffset < squad.anchors.count; ++anchorOffset) {
            const format::SquadAnchor& anchor =
                source.squadAnchors[squad.anchors.first + anchorOffset];
            output.append("            { x = ");
            append_float(output, anchor.positionBits[0]);
            output.append(", y = ");
            append_float(output, anchor.positionBits[1]);
            output.append(", z = ");
            append_float(output, anchor.positionBits[2]);
            output.append(", point = ");
            append_uint(output, anchor.pointOrdinal);
            output.append(" },\n");
        }
        output.append("        } },\n");
        squadConstants.append("    ");
        squadConstants.append(key);
        squadConstants.append(" = ");
        append_string(squadConstants, id);
        squadConstants.append(",\n");
    }
    output.append("    },\n    scenes = {\n");
    std::unordered_set<std::string> sceneKeys;
    std::string sceneConstants = "mission.Scene = {\n";
    if (!range_inside(scenario.occurrences, source.occurrences.size())) {
        return false;
    }
    for (std::size_t occurrenceOffset = 0; occurrenceOffset < scenario.occurrences.count;
         ++occurrenceOffset) {
        const std::uint32_t occurrenceRow =
            scenario.occurrences.first + static_cast<std::uint32_t>(occurrenceOffset);
        const format::Occurrence& occurrence = source.occurrences[occurrenceRow];
        if (occurrence.objectIndex >= source.objects.size()) {
            return false;
        }
        const format::Object& object = source.objects[occurrence.objectIndex];
        if (!range_inside(object.slots, source.slots.size())) {
            return false;
        }
        for (std::size_t slotOffset = 0; slotOffset < object.slots.count; ++slotOffset) {
            const std::uint32_t slotRow =
                object.slots.first + static_cast<std::uint32_t>(slotOffset);
            const auto found = index.scenesBySlot.find(slotRow);
            const auto slotKey = slotKeyByRow.find(slotRow);
            if (found == index.scenesBySlot.end() || found->second.size() != 1
                || slotKey == slotKeyByRow.end()) {
                continue;
            }
            const format::Slot& slot = source.slots[slotRow];
            const format::AuthoredSceneResource& scene =
                source.authoredSceneResources[found->second.front()];
            std::string id;
            if (!scene_symbol_id(source, occurrence, slot, id)) {
                return false;
            }
            std::string key = slotKey->second;
            if (!sceneKeys.insert(key).second) {
                key.append("__");
                key.append(identifier(text(source, occurrence.id), true));
                if (!sceneKeys.insert(key).second) {
                    std::array<char, 16> suffix{};
                    const int length =
                        std::snprintf(suffix.data(), suffix.size(), "_%08X", occurrenceRow);
                    key.append(suffix.data(), static_cast<std::size_t>(length));
                    if (!sceneKeys.insert(key).second) {
                        return false;
                    }
                }
            }
            output.append("    ");
            output.append(key);
            output.append(" = ");
            output.append("{ id = ");
            append_string(output, id);
            output.append(", slot = ");
            append_uint(output, slotRow);
            output.append(", config_tag = ");
            append_hex(output, scene.configTag);
            output.append(", resource_tag = ");
            append_hex(output, scene.resourceTag);
            output.append(" },\n");
            sceneConstants.append("    ");
            sceneConstants.append(key);
            sceneConstants.append(" = ");
            append_string(sceneConstants, id);
            sceneConstants.append(",\n");
        }
    }
    output.append("    },\n    tasks = {\n");
    std::unordered_set<std::string> taskKeys;
    std::string taskConstants = "mission.Task = {\n";
    std::vector<std::uint32_t> tasks;
    for (const std::uint32_t slot : slots) {
        const auto found = index.tasksBySlot.find(slot);
        if (found != index.tasksBySlot.end()) {
            tasks.insert(tasks.end(), found->second.begin(), found->second.end());
        }
    }
    std::sort(tasks.begin(), tasks.end());
    tasks.erase(std::unique(tasks.begin(), tasks.end()), tasks.end());
    for (const std::uint32_t taskRow : tasks) {
        const format::TaskTarget& task = source.taskTargets[taskRow];
        const std::string_view id = text(source, task.id);
        const std::string key = append_unique_key(output, taskKeys, id, task.taskSlotIndex);
        output.append("{ id = ");
        append_string(output, id);
        output.append(", task_slot = ");
        append_uint(output, task.taskSlotIndex);
        output.append(", objective_slot = ");
        append_uint(output, task.objectiveSlotIndex);
        output.append(", objective_bit = ");
        append_uint(output, task.bitIndex);
        output.append(", config_tag = ");
        append_hex(output, task.configTag);
        output.append(" },\n");
        taskConstants.append("    ");
        taskConstants.append(key);
        taskConstants.append(" = ");
        append_string(taskConstants, id);
        taskConstants.append(",\n");
    }
    output.append("    },\n}\n\n");
    stateConstants.append("}\n");
    slotConstants.append("}\n");
    squadConstants.append("}\n");
    sceneConstants.append("}\n");
    taskConstants.append("}\n");
    std::string taskGroupConstants = "mission.TaskGroup = {\n";
    for (const std::uint32_t slot : slots) {
        const auto found = index.combatGroupsBySlot.find(slot);
        const auto key = slotKeyByRow.find(slot);
        if (found == index.combatGroupsBySlot.end() || key == slotKeyByRow.end()) {
            continue;
        }
        taskGroupConstants.append("    ");
        taskGroupConstants.append(key->second);
        taskGroupConstants.append(" = {\n");
        for (const std::uint32_t row : found->second) {
            const format::CombatObjectiveGroup& group = source.combatObjectiveGroups[row];
            taskGroupConstants.append("        GROUP_");
            append_uint(taskGroupConstants, group.groupIndex);
            taskGroupConstants.append(" = { slot_row = ");
            append_uint(taskGroupConstants, group.slotIndex);
            taskGroupConstants.append(", group_index = ");
            append_uint(taskGroupConstants, group.groupIndex);
            taskGroupConstants.append(" },\n");
        }
        taskGroupConstants.append("    },\n");
    }
    taskGroupConstants.append("}\n");
    std::string abilityConstants = "mission.ActorAbility = {\n";
    for (const std::uint32_t slot : slots) {
        const auto found = index.abilitiesBySlot.find(slot);
        const auto key = slotKeyByRow.find(slot);
        if (found == index.abilitiesBySlot.end() || key == slotKeyByRow.end()) {
            continue;
        }
        abilityConstants.append("    ").append(key->second).append(" = {\n");
        std::uint32_t previousGroup = 0;
        for (const std::uint32_t row : found->second) {
            const format::ActorAbility& ability = source.actorAbilities[row];
            std::array<char, 32> symbol{};
            if (ability.groupHash != previousGroup) {
                if (previousGroup != 0) {
                    abilityConstants.append("        },\n");
                }
                std::snprintf(symbol.data(), symbol.size(), "GROUP_%08X", ability.groupHash);
                abilityConstants.append("        ").append(symbol.data()).append(" = {\n");
                previousGroup = ability.groupHash;
            }
            std::snprintf(symbol.data(), symbol.size(), "KEY_%08X", ability.requestHash);
            abilityConstants.append("            ")
                .append(symbol.data())
                .append(" = { slot_row = ");
            append_uint(abilityConstants, ability.slotIndex);
            abilityConstants.append(", group_hash = ");
            append_hex(abilityConstants, ability.groupHash);
            abilityConstants.append(", request_hash = ");
            append_hex(abilityConstants, ability.requestHash);
            abilityConstants.append(" },\n");
        }
        if (previousGroup != 0) {
            abilityConstants.append("        },\n");
        }
        abilityConstants.append("    },\n");
    }
    abilityConstants.append("}\n");
    std::string dialogueCueConstants = "mission.DialogueCue = {\n";
    std::string dialogueCueTextConstants = "mission.DialogueCueVariants = {\n";
    std::string dialogueDefinitionConstants = "mission.DialogueDefinition = {\n";
    for (const std::uint32_t slot : slots) {
        const auto found = index.dialogueBySlot.find(slot);
        const auto slotKey = slotKeyByRow.find(slot);
        if (slotKey == slotKeyByRow.end() || slot >= source.slots.size()) {
            continue;
        }
        const format::Slot& definition = source.slots[slot];
        if ((definition.flags & format::kSlotDialogueCuesExact) == 0 || definition.reserved == 0) {
            continue;
        }
        dialogueCueConstants.append("    ");
        dialogueCueConstants.append(slotKey->second);
        dialogueCueConstants.append(" = {\n");
        dialogueCueTextConstants.append("    ");
        dialogueCueTextConstants.append(slotKey->second);
        dialogueCueTextConstants.append(" = {\n");
        dialogueDefinitionConstants.append("    ");
        dialogueDefinitionConstants.append(slotKey->second);
        dialogueDefinitionConstants.append(" = {\n");
        for (std::uint32_t cue = 0; cue < definition.reserved; ++cue) {
            dialogueCueConstants.append("        CUE_");
            append_uint(dialogueCueConstants, cue);
            dialogueCueConstants.append(" = ");
            append_uint(dialogueCueConstants, cue);
            dialogueCueConstants.append(",\n");

            if (found != index.dialogueBySlot.end()) {
                for (const std::uint32_t rowIndex : found->second) {
                    const format::DialogueCueText& row = source.dialogueCueTexts[rowIndex];
                    if (row.cueIndex == cue) {
                        dialogueDefinitionConstants.append("        [");
                        append_uint(dialogueDefinitionConstants, cue);
                        dialogueDefinitionConstants.append("] = ");
                        append_hex(dialogueDefinitionConstants, row.definitionHash);
                        dialogueDefinitionConstants.append(",\n");
                        break;
                    }
                }
            }

            dialogueCueTextConstants.append("        [");
            append_uint(dialogueCueTextConstants, cue);
            dialogueCueTextConstants.append("] = {");
            std::unordered_set<std::string_view> emittedTexts;
            if (found != index.dialogueBySlot.end()) {
                for (const std::uint32_t rowIndex : found->second) {
                    const format::DialogueCueText& row = source.dialogueCueTexts[rowIndex];
                    const std::string_view candidate = text(source, row.text);
                    if (row.cueIndex != cue || candidate.empty()
                        || !emittedTexts.emplace(candidate).second) {
                        continue;
                    }
                    dialogueCueTextConstants.push_back('\n');
                    dialogueCueTextConstants.append("            ");
                    append_string(dialogueCueTextConstants, candidate);
                    dialogueCueTextConstants.push_back(',');
                }
            }
            if (!emittedTexts.empty()) {
                dialogueCueTextConstants.append("\n        ");
            }
            dialogueCueTextConstants.append("},\n");
        }
        dialogueCueConstants.append("    },\n");
        dialogueCueTextConstants.append("    },\n");
        dialogueDefinitionConstants.append("    },\n");
    }
    dialogueCueConstants.append("}\n");
    dialogueCueTextConstants.append("}\n");
    dialogueDefinitionConstants.append("}\n");
    std::unordered_set<std::string> directiveKeys;
    std::string directiveConstants = "mission.Directive = {\n";
    for (const std::uint32_t slot : slots) {
        const auto found = index.directivesBySlot.find(slot);
        if (found == index.directivesBySlot.end()) {
            continue;
        }
        for (const std::uint32_t rowIndex : found->second) {
            const format::DirectiveElement& row = source.directiveElements[rowIndex];
            (void)append_unique_key(
                directiveConstants, directiveKeys, text(source, row.title), row.nameHash);
            directiveConstants.append("{ id = ");
            append_string(directiveConstants, text(source, row.id));
            directiveConstants.append(", slot_row = ");
            append_uint(directiveConstants, row.slotIndex);
            directiveConstants.append(", name_hash = ");
            append_hex(directiveConstants, row.nameHash);
            directiveConstants.append(", element = ");
            append_uint(directiveConstants, static_cast<std::uint32_t>(row.elementIndex));
            directiveConstants.append(", title = ");
            append_string(directiveConstants, text(source, row.title));
            directiveConstants.append(", description = ");
            append_string(directiveConstants, text(source, row.description));
            directiveConstants.append(" },\n");
        }
    }
    directiveConstants.append("}\n");
    // One entry per type-42 sensor: the distinct state names its target squad's members declare.
    std::string performanceConstants = "mission.PerformanceState = {\n";
    for (const std::uint32_t slotRow : slots) {
        const format::Slot& slot = source.slots[slotRow];
        if (slot.slotType != format::kPerformanceSlotType
            || slot.authSchema != format::kPerformanceAuthSchema) {
            continue;
        }
        std::uint32_t squadSlot = format::kAbsentIndex;
        std::size_t edges = 0;
        for (const format::AuthoredSceneSquadEdge& edge : source.authoredSceneSquadEdges) {
            if (edge.sceneSlotIndex == slotRow
                && (edge.flags & format::kAuthoredSceneSquadPerformanceTargetExact) != 0) {
                squadSlot = edge.squadSlotIndex;
                ++edges;
            }
        }
        if (edges != 1) {
            continue;
        }
        std::vector<std::uint32_t> names;
        for (const format::Squad& squad : source.squads) {
            if (squad.scenarioIndex != scenarioIndex || squad.slotIndex != squadSlot
                || !range_inside(squad.members, source.squadMembers.size())) {
                continue;
            }
            for (std::size_t offset = 0; offset < squad.members.count; ++offset) {
                const std::uint32_t actorClass =
                    source.squadMembers[squad.members.first + offset].actorClassIndex;
                for (const format::ActorStateName& row : source.actorStateNames) {
                    if (row.actorClassIndex == actorClass
                        && std::find(names.begin(), names.end(), row.nameHash) == names.end()) {
                        names.push_back(row.nameHash);
                    }
                }
            }
        }
        if (names.empty()) {
            continue;
        }
        performanceConstants.append("    ");
        performanceConstants.append(slotKeyByRow.at(slotRow));
        performanceConstants.append(" = {\n");
        for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
            std::array<char, 24> key{};
            const int length = std::snprintf(
                key.data(), key.size(), "STATE_%08X", static_cast<unsigned>(names[ordinal]));
            performanceConstants.append("        ");
            performanceConstants.append(key.data(), static_cast<std::size_t>(length));
            performanceConstants.append(" = { slot_row = ");
            append_uint(performanceConstants, slotRow);
            performanceConstants.append(", name_hash = ");
            append_hex(performanceConstants, names[ordinal]);
            performanceConstants.append(", ordinal = ");
            append_uint(performanceConstants, static_cast<std::uint32_t>(ordinal + 1));
            performanceConstants.append(" },\n");
        }
        performanceConstants.append("    },\n");
    }
    performanceConstants.append("}\n");
    std::string authSlotConstants = "mission.Auth = {\n";
    for (std::size_t authRow = 0; authRow < authConstants.size(); ++authRow) {
        if (authConstants[authRow].empty()) {
            continue;
        }
        authSlotConstants.append("    ");
        authSlotConstants.append(auth_catalog::kTypes[authRow].name);
        authSlotConstants.append(" = {\n");
        authSlotConstants.append(authConstants[authRow]);
        authSlotConstants.append("    },\n");
    }
    authSlotConstants.append("}\n");
    std::string actorMessageConstants = "mission.ActorMessage = {\n";
    for (const format::ActorMessageSchema& row : source.actorMessageSchemas) {
        actorMessageConstants.append("    ");
        actorMessageConstants.append(identifier(text(source, row.name), true));
        actorMessageConstants.append(" = { definition = ");
        append_hex(actorMessageConstants, row.definitionHandle);
        actorMessageConstants.append(", durable_key = ");
        append_hex(actorMessageConstants, row.durableKey);
        actorMessageConstants.append(", owner_class = ");
        append_hex(actorMessageConstants, row.ownerClass);
        actorMessageConstants.append(", handler_slot = ");
        append_uint(actorMessageConstants, row.handlerSlot);
        actorMessageConstants.append(", body_type = ");
        append_uint(actorMessageConstants, row.bodyType);
        actorMessageConstants.append(" },\n");
    }
    actorMessageConstants.append("}\n");
    std::string actorCommandConstants = "mission.ActorCommand = {\n";
    std::string actorCommandDefinitions = "mission.ActorCommandDefinition = {\n";
    std::string factionConstants = "mission.Faction = {\n";
    bool factionValuesWritten = false;
    for (const format::ActorCommandDefinition& row : source.actorCommandDefinitions) {
        const std::string commandName = identifier(text(source, row.name), true);
        actorCommandConstants.append("    ");
        actorCommandConstants.append(commandName);
        actorCommandConstants.append(" = ");
        append_uint(actorCommandConstants, row.selector);
        actorCommandConstants.append(",\n");
        actorCommandDefinitions.append("    ");
        actorCommandDefinitions.append(commandName);
        actorCommandDefinitions.append(" = { selector = mission.ActorCommand.");
        actorCommandDefinitions.append(commandName);
        actorCommandDefinitions.append(", payload = ");
        append_hex(actorCommandDefinitions, row.payloadHandle);
        actorCommandDefinitions.append(" },\n");
        if (row.effect == format::ActorCommandEffect::setFaction && !factionValuesWritten) {
            factionConstants.append("    ");
            factionConstants.append(identifier(text(source, row.factionNoneName), true));
            factionConstants.append(" = ");
            append_int(factionConstants, row.factionNone);
            factionConstants.append(",\n    ");
            factionConstants.append(identifier(text(source, row.factionRemovedName), true));
            factionConstants.append(" = ");
            append_int(factionConstants, row.factionRemoved);
            factionConstants.append(",\n    ");
            factionConstants.append(identifier(text(source, row.factionHostileToAllName), true));
            factionConstants.append(" = ");
            append_int(factionConstants, row.factionHostileToAll);
            factionConstants.append(",\n");
            factionValuesWritten = true;
        }
    }
    actorCommandConstants.append("}\n");
    actorCommandDefinitions.append("}\n");
    factionConstants.append("}\n");
    std::string simulationEventConstants = "mission.SimulationEvent = {\n";
    std::string simulationEventDefinitions = "mission.SimulationEventDefinition = {\n";
    for (const format::SimulationEventDefinition& row : source.simulationEventDefinitions) {
        const std::string eventName = identifier(text(source, row.name), true);
        simulationEventConstants.append("    ");
        simulationEventConstants.append(eventName);
        simulationEventConstants.append(" = ");
        append_uint(simulationEventConstants, row.eventType);
        simulationEventConstants.append(",\n");
        simulationEventDefinitions.append("    ");
        simulationEventDefinitions.append(eventName);
        simulationEventDefinitions.append(" = { event_type = mission.SimulationEvent.");
        simulationEventDefinitions.append(eventName);
        simulationEventDefinitions.append(", primary_schema = ");
        if (row.primarySchema == format::kAbsentIndex) {
            simulationEventDefinitions.append("nil");
        } else {
            append_hex(simulationEventDefinitions, row.primarySchema);
        }
        simulationEventDefinitions.append(", secondary_schema = ");
        if (row.secondarySchema == format::kAbsentIndex) {
            simulationEventDefinitions.append("nil");
        } else {
            append_hex(simulationEventDefinitions, row.secondarySchema);
        }
        simulationEventDefinitions.append(" },\n");
    }
    simulationEventConstants.append("}\n");
    simulationEventDefinitions.append("}\n");
    std::string runtimeFieldTypes = "mission.RuntimeFieldType = {\n";
    // Lua name published for each runtime codec family.
    constexpr std::array<std::pair<format::RuntimeCodecFamily, std::string_view>, 3> kFamilies{{
        {format::RuntimeCodecFamily::activity, "ACTIVITY"},
        {format::RuntimeCodecFamily::sobjectModeZero, "SOBJECT_MODE_ZERO"},
        {format::RuntimeCodecFamily::sobjectModeOne, "SOBJECT_MODE_ONE"},
    }};
    for (const auto& [family, familyName] : kFamilies) {
        runtimeFieldTypes.append("    ");
        runtimeFieldTypes.append(familyName);
        runtimeFieldTypes.append(" = {\n");
        for (const format::RuntimeTypeDefinition& row : source.runtimeTypeDefinitions) {
            if ((row.codecFamilies & static_cast<std::uint32_t>(family)) == 0) {
                continue;
            }
            runtimeFieldTypes.append("        ");
            runtimeFieldTypes.append(identifier(text(source, row.name), true));
            runtimeFieldTypes.append(" = ");
            append_uint(runtimeFieldTypes, row.typeCode);
            runtimeFieldTypes.append(",\n");
        }
        runtimeFieldTypes.append("    },\n");
    }
    runtimeFieldTypes.append("}\n");
    output.append(stateConstants);
    output.append(slotConstants);
    output.append(authSlotConstants);
    output.append(actorMessageConstants);
    output.append(actorCommandConstants);
    output.append(actorCommandDefinitions);
    output.append(factionConstants);
    output.append(simulationEventConstants);
    output.append(simulationEventDefinitions);
    output.append(runtimeFieldTypes);
    output.append(squadConstants);
    output.append(sceneConstants);
    output.append(taskConstants);
    output.append(taskGroupConstants);
    output.append(abilityConstants);
    output.append(dialogueCueConstants);
    output.append(dialogueCueTextConstants);
    output.append(dialogueDefinitionConstants);
    output.append(directiveConstants);
    output.append(performanceConstants);
    const auto world = index.worldsByScenarioTag.find(scenario.tag);
    if (world != index.worldsByScenarioTag.end()
        && world->second < source.scenarioWorldSources.size()) {
        output.append(source.scenarioWorldSources[world->second].source);
    }
    output.append("\nreturn mission\n");
    module.source = std::move(output);
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
