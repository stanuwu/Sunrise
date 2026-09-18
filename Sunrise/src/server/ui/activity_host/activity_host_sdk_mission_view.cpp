#include "activity_host_sdk_mission_view.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../activity/activity_sdk_mission_runtime.h"
#include "activity_host_sdk_behavior_view.h"
#include "activity_host_sdk_mission_text.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::sdk_mission_view {
namespace {

namespace mission = server::activity::activity_sdk_mission;
namespace sdk = state::activity_sdk;
namespace section = core::ui::components::section;
std::array<char, 256> g_search{};
std::uint64_t g_actionGeneration{};
std::uint32_t g_actionScenario{sdk::format::kAbsentIndex};
std::uint32_t g_sceneActionOccurrence{sdk::format::kAbsentIndex};
std::uint32_t g_sceneActionSlot{sdk::format::kAbsentIndex};
mission::SceneStatus g_sceneActionStatus{mission::SceneStatus::ready};
bool g_hasSceneActionStatus{};
std::uint32_t g_dialogueActionOccurrence{sdk::format::kAbsentIndex};
std::uint32_t g_dialogueActionSlot{sdk::format::kAbsentIndex};
std::uint16_t g_dialogueActionCue{};
mission::SceneStatus g_dialogueActionStatus{mission::SceneStatus::ready};
bool g_hasDialogueActionStatus{};
std::uint32_t g_directiveActionOccurrence{sdk::format::kAbsentIndex};
std::uint32_t g_directiveActionSlot{sdk::format::kAbsentIndex};
std::uint32_t g_directiveActionHash{};
std::int32_t g_directiveActionElement{};
mission::SceneStatus g_directiveActionStatus{mission::SceneStatus::ready};
bool g_hasDirectiveActionStatus{};
/** One filtered scene keeps its exact occurrence and global slot rows. */
struct SceneBrowserRow final {
    std::uint32_t occurrenceRow{sdk::format::kAbsentIndex};
    std::uint32_t slotRow{sdk::format::kAbsentIndex};
    mission::SceneStatus availability{mission::SceneStatus::invalidView};
};

std::vector<SceneBrowserRow> g_visibleScenes{};

/** @return The global row of one borrowed slot or the absent marker. */
[[nodiscard]] std::uint32_t global_slot_row(const sdk::Catalog& catalog,
                                            const sdk::format::Slot& slot) noexcept {
    const auto slots = catalog.slots();
    if (slots.empty() || &slot < slots.data() || &slot >= slots.data() + slots.size()) {
        return sdk::format::kAbsentIndex;
    }
    return static_cast<std::uint32_t>(&slot - slots.data());
}

/** Checks every displayed name and numeric identity for one authored scene. */
[[nodiscard]] bool scene_matches(const sdk::Catalog& catalog,
                                 const sdk::format::Occurrence& occurrence,
                                 std::uint32_t occurrenceRow,
                                 const sdk::format::Object& object,
                                 const sdk::format::Slot& slot,
                                 std::uint32_t slotRow,
                                 mission::SceneStatus availability) noexcept {
    const std::string_view query = search_text();
    if (query.empty() || contains_folded(catalog.string(occurrence.id), query)
        || contains_folded(catalog.string(occurrence.contextRegistryKey), query)
        || contains_folded(catalog.string(occurrence.registryId), query)
        || contains_folded(catalog.string(occurrence.entryId), query)
        || contains_folded(catalog.string(object.id), query)
        || contains_folded(catalog.string(slot.id), query)
        || contains_folded(catalog.string(slot.name), query)
        || contains_folded(catalog.string(slot.senseSchemaId), query)
        || contains_folded(catalog.string(slot.authSchemaId), query)
        || contains_folded(mission::status_name(availability), query)) {
        return true;
    }

    std::uint32_t configTag = 0;
    std::uint32_t resourceTag = 0;
    const auto resources = sdk::slot_authored_scene_resources(catalog, slot);
    for (const sdk::format::AuthoredSceneResource& resource : resources) {
        if (contains_folded(catalog.string(resource.id), query)) {
            return true;
        }
        configTag = resource.configTag;
        resourceTag = resource.resourceTag;
    }

    std::uint32_t linkedSlotRow = sdk::format::kAbsentIndex;
    std::uint32_t linkConfigTag = 0;
    std::uint32_t targetObjectKey = 0;
    const auto edges = sdk::slot_authored_scene_squad_edges(catalog, slot);
    for (const sdk::format::AuthoredSceneSquadEdge& edge : edges) {
        if (contains_folded(catalog.string(edge.id), query)) {
            return true;
        }
        const sdk::format::Slot* const linked =
            sdk::authored_scene_linked_squad_slot(catalog, edge);
        if (linked != nullptr
            && (contains_folded(catalog.string(linked->id), query)
                || contains_folded(catalog.string(linked->name), query))) {
            return true;
        }
        linkedSlotRow = edge.squadSlotIndex;
        linkConfigTag = edge.configTag;
        targetObjectKey = edge.targetObjectKey;
    }

    std::array<char, 768> scalars{};
    const int length = std::snprintf(
        scalars.data(),
        scalars.size(),
        "occurrence %u object row %u tag 0x%08X key 0x%08X slot row %u index %u type %u "
        "class 0x%08X sense 0x%08X auth 0x%08X config 0x%08X resource 0x%08X resources %zu "
        "linked squad slot row %u link config 0x%08X target key 0x%08X edges %zu",
        static_cast<unsigned>(occurrenceRow),
        static_cast<unsigned>(occurrence.objectIndex),
        static_cast<unsigned>(object.objectTag),
        static_cast<unsigned>(object.objectKey),
        static_cast<unsigned>(slotRow),
        static_cast<unsigned>(slot.slotIndex),
        static_cast<unsigned>(slot.slotType),
        static_cast<unsigned>(slot.componentClass),
        static_cast<unsigned>(slot.senseSchema),
        static_cast<unsigned>(slot.authSchema),
        static_cast<unsigned>(configTag),
        static_cast<unsigned>(resourceTag),
        resources.size(),
        static_cast<unsigned>(linkedSlotRow),
        static_cast<unsigned>(linkConfigTag),
        static_cast<unsigned>(targetObjectKey),
        edges.size());
    return length > 0
           && contains_folded(
               std::string_view(scalars.data(),
                                (std::min)(scalars.size() - 1, static_cast<std::size_t>(length))),
               query);
}

/** Builds type-43 rows from only the selected state-ordinal-0 occurrence set. */
[[nodiscard]] bool materialize_visible_scenes(const sdk::BoundView& view,
                                              std::uint32_t stateRow) noexcept {
    try {
        g_visibleScenes.clear();
        if (view.catalog == nullptr || stateRow >= view.catalog->states().size()) {
            return false;
        }
        const sdk::Catalog& catalog = *view.catalog;
        const auto occurrences = catalog.occurrences();
        const auto objects = catalog.objects();
        for (std::size_t occurrenceRow = 0; occurrenceRow < occurrences.size(); ++occurrenceRow) {
            const sdk::format::Occurrence& occurrence = occurrences[occurrenceRow];
            if (occurrence.scenarioIndex != view.scenarioRow || occurrence.stateIndex != stateRow
                || occurrence.objectIndex >= objects.size()) {
                continue;
            }
            const sdk::format::Object& object = objects[occurrence.objectIndex];
            for (const sdk::format::Slot& slot : sdk::object_slots(catalog, object)) {
                if (slot.slotType != sdk::format::kAuthoredSceneSlotType) {
                    continue;
                }
                const std::uint32_t slotRow = global_slot_row(catalog, slot);
                const mission::SceneStatus availability =
                    slotRow != sdk::format::kAbsentIndex
                        ? mission::authored_scene_availability(
                              view, static_cast<std::uint32_t>(occurrenceRow), slotRow)
                        : mission::SceneStatus::invalidSlot;
                if (slotRow != sdk::format::kAbsentIndex
                    && scene_matches(catalog,
                                     occurrence,
                                     static_cast<std::uint32_t>(occurrenceRow),
                                     object,
                                     slot,
                                     slotRow,
                                     availability)) {
                    g_visibleScenes.push_back(
                        {static_cast<std::uint32_t>(occurrenceRow), slotRow, availability});
                }
            }
        }
        return true;
    } catch (...) {
        g_visibleScenes.clear();
        return false;
    }
}

/** Draws searchable exact type-43 rows and their manual one-generation action. */
void draw_authored_scenes(const sdk::BoundView& view, const mission::Snapshot& snapshot) noexcept {
    if (!materialize_visible_scenes(view, snapshot.plan.stateRow)) {
        ImGui::TextDisabled("No scene in this state.");
        return;
    }
    ImGui::TextDisabled("%zu listed in state row %u",
                        g_visibleScenes.size(),
                        static_cast<unsigned>(snapshot.plan.stateRow));
    if (g_visibleScenes.empty()) {
        ImGui::TextDisabled("No scene matches.");
        return;
    }

    const sdk::Catalog& catalog = *view.catalog;
    const auto occurrences = catalog.occurrences();
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    const auto states = catalog.states();
    const auto bubbles = catalog.bubbles();
    if (!ImGui::BeginTable("##sdk_mission_scenes",
                           9,
                           kSceneTableFlags,
                           table_layout::size(g_visibleScenes.size()))) {
        return;
    }
    ImGui::TableSetupColumn("scene", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("occurrence");
    ImGui::TableSetupColumn("state");
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("config");
    ImGui::TableSetupColumn("resource");
    ImGui::TableSetupColumn("linked squad", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("availability");
    ImGui::TableSetupColumn("action");
    table_layout::frozen_headers();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_visibleScenes.size()));
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            const SceneBrowserRow row = g_visibleScenes[static_cast<std::size_t>(visible)];
            if (row.occurrenceRow >= occurrences.size() || row.slotRow >= slots.size()) {
                continue;
            }
            const sdk::format::Occurrence& occurrence = occurrences[row.occurrenceRow];
            const sdk::format::Slot& slot = slots[row.slotRow];
            if (occurrence.objectIndex >= objects.size()) {
                continue;
            }
            const sdk::format::Object& object = objects[occurrence.objectIndex];
            const auto resources = sdk::slot_authored_scene_resources(catalog, slot);
            const sdk::format::AuthoredSceneResource* const resource =
                resources.size() == 1 ? &resources.front() : nullptr;
            const auto edges = sdk::slot_authored_scene_squad_edges(catalog, slot);
            const sdk::format::AuthoredSceneSquadEdge* const edge =
                edges.size() == 1 ? &edges.front() : nullptr;
            const sdk::format::Slot* const linkedSquad =
                edge != nullptr ? sdk::authored_scene_linked_squad_slot(catalog, *edge) : nullptr;
            const mission::SceneStatus available = row.availability;
            const std::string_view slotName = display_text(catalog, slot.name);

            ImGui::PushID(static_cast<int>(row.occurrenceRow));
            ImGui::PushID(static_cast<int>(row.slotRow));
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::Text("%.*s\n%.*s",
                        print_length(slotName),
                        slotName.data(),
                        print_length(display_text(catalog, object.id)),
                        display_text(catalog, object.id).data());
            ImGui::TableNextColumn();
            ImGui::Text("row %u\n%.*s",
                        static_cast<unsigned>(row.occurrenceRow),
                        print_length(display_text(catalog, occurrence.id)),
                        display_text(catalog, occurrence.id).data());
            ImGui::TableNextColumn();
            if (occurrence.stateIndex >= states.size()
                || occurrence.bubbleIndex >= bubbles.size()) {
                ImGui::TextDisabled("invalid");
            } else {
                ImGui::Text("row %u\n%u/%u",
                            static_cast<unsigned>(occurrence.stateIndex),
                            static_cast<unsigned>(bubbles[occurrence.bubbleIndex].bubbleOrdinal),
                            static_cast<unsigned>(states[occurrence.stateIndex].stateOrdinal));
            }
            ImGui::TableNextColumn();
            ImGui::Text("%u / type %u\nrow %u",
                        static_cast<unsigned>(slot.slotIndex),
                        static_cast<unsigned>(slot.slotType),
                        static_cast<unsigned>(row.slotRow));
            ImGui::TableNextColumn();
            if (resource == nullptr) {
                ImGui::TextDisabled("%zu rows", resources.size());
            } else {
                ImGui::Text("0x%08X", static_cast<unsigned>(resource->configTag));
            }
            ImGui::TableNextColumn();
            if (resource == nullptr) {
                ImGui::TextDisabled("unavailable");
            } else {
                ImGui::Text("0x%08X\n%.*s",
                            static_cast<unsigned>(resource->resourceTag),
                            print_length(display_text(catalog, resource->id)),
                            display_text(catalog, resource->id).data());
            }
            ImGui::TableNextColumn();
            if (edge == nullptr) {
                ImGui::TextDisabled("%zu edges", edges.size());
            } else if (linkedSquad == nullptr) {
                ImGui::TextDisabled("invalid slot row %u",
                                    static_cast<unsigned>(edge->squadSlotIndex));
            } else {
                ImGui::Text("%.*s\n%u / type %u / row %u\nconfig 0x%08X / key 0x%08X",
                            print_length(display_text(catalog, linkedSquad->name)),
                            display_text(catalog, linkedSquad->name).data(),
                            static_cast<unsigned>(linkedSquad->slotIndex),
                            static_cast<unsigned>(linkedSquad->slotType),
                            static_cast<unsigned>(edge->squadSlotIndex),
                            static_cast<unsigned>(edge->configTag),
                            static_cast<unsigned>(edge->targetObjectKey));
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(mission::status_name(available));
            if (g_hasSceneActionStatus && g_sceneActionOccurrence == row.occurrenceRow
                && g_sceneActionSlot == row.slotRow) {
                ImGui::TextDisabled("last: %s", mission::status_name(g_sceneActionStatus));
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(available != mission::SceneStatus::ready);
            if (ImGui::Button("Advance")) {
                g_sceneActionOccurrence = row.occurrenceRow;
                g_sceneActionSlot = row.slotRow;
                g_sceneActionStatus =
                    mission::activate_authored_scene(view, row.occurrenceRow, row.slotRow);
                g_hasSceneActionStatus = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", mission::status_name(available));
            }
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

/** Draws type-53 cues and the localized variants selected by each authored cue definition. */
void draw_dialogues(const sdk::BoundView& view, const mission::Snapshot& snapshot) noexcept {
    if (view.catalog == nullptr || snapshot.plan.stateRow >= view.catalog->states().size()) {
        ImGui::TextDisabled("Dialogue rows unavailable.");
        return;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto occurrences = catalog.occurrences();
    const auto objects = catalog.objects();
    const std::string_view query = search_text();
    std::size_t sensorCount = 0;
    for (std::uint32_t occurrenceRow = 0; occurrenceRow < occurrences.size(); ++occurrenceRow) {
        const sdk::format::Occurrence& occurrence = occurrences[occurrenceRow];
        if (occurrence.scenarioIndex != view.scenarioRow
            || occurrence.stateIndex != snapshot.plan.stateRow
            || occurrence.objectIndex >= objects.size()) {
            continue;
        }
        const sdk::format::Object& object = objects[occurrence.objectIndex];
        for (const sdk::format::Slot& slot : sdk::object_slots(catalog, object)) {
            if (slot.slotType != sdk::format::kDialogueSlotType
                || (slot.flags & sdk::format::kSlotDialogueCuesExact) == 0 || slot.reserved == 0
                || slot.reserved > sdk::format::kDialogueMaximumCueCount) {
                continue;
            }
            const std::uint32_t slotRow = global_slot_row(catalog, slot);
            if (slotRow == sdk::format::kAbsentIndex
                || (!query.empty() && !contains_folded(catalog.string(slot.name), query)
                    && !contains_folded(catalog.string(slot.id), query)
                    && !contains_folded(catalog.string(object.id), query)
                    && !contains_folded(catalog.string(occurrence.id), query))) {
                continue;
            }
            ++sensorCount;
            ImGui::PushID(static_cast<int>(occurrenceRow));
            ImGui::PushID(static_cast<int>(slotRow));
            const std::string_view name = display_text(catalog, slot.name);
            const bool expanded = ImGui::TreeNodeEx("##dialogue_sensor",
                                                    ImGuiTreeNodeFlags_SpanAvailWidth,
                                                    "%.*s  (%u cues)",
                                                    print_length(name),
                                                    name.data(),
                                                    static_cast<unsigned>(slot.reserved));
            if (expanded) {
                ImGui::TextDisabled("%.*s / occurrence %u / slot %u",
                                    print_length(display_text(catalog, object.id)),
                                    display_text(catalog, object.id).data(),
                                    static_cast<unsigned>(occurrenceRow),
                                    static_cast<unsigned>(slot.slotIndex));
                for (std::uint32_t cueRow = 0; cueRow < slot.reserved; ++cueRow) {
                    // The cue index is a 16-bit wire field, so the row is narrowed once here.
                    const auto cue = static_cast<std::uint16_t>(cueRow);
                    ImGui::PushID(static_cast<int>(cueRow));
                    const mission::SceneStatus available =
                        mission::dialogue_cue_availability(view, occurrenceRow, slotRow, cue);
                    ImGui::BeginDisabled(available != mission::SceneStatus::ready);
                    if (ImGui::Button("Play cue")) {
                        g_dialogueActionOccurrence = occurrenceRow;
                        g_dialogueActionSlot = slotRow;
                        g_dialogueActionCue = cue;
                        g_dialogueActionStatus =
                            mission::play_dialogue_cue(view, occurrenceRow, slotRow, cue);
                        g_hasDialogueActionStatus = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    std::uint32_t definitionHash = 0;
                    for (const sdk::format::DialogueCueText& row : catalog.dialogue_cue_texts()) {
                        if (row.slotIndex == slotRow && row.cueIndex == cue) {
                            definitionHash = row.definitionHash;
                            break;
                        }
                    }
                    ImGui::Text("Cue %u  definition %08X",
                                static_cast<unsigned>(cue),
                                static_cast<unsigned>(definitionHash));
                    ImGui::Indent();
                    bool hasCandidate = false;
                    const auto dialogueRows = catalog.dialogue_cue_texts();
                    for (std::size_t rowIndex = 0; rowIndex < dialogueRows.size(); ++rowIndex) {
                        const sdk::format::DialogueCueText& row = dialogueRows[rowIndex];
                        if (row.slotIndex != slotRow || row.cueIndex != cue) {
                            continue;
                        }
                        const std::string_view candidate = catalog.string(row.text);
                        if (candidate.empty()) {
                            continue;
                        }
                        bool duplicate = false;
                        for (std::size_t previous = 0; previous < rowIndex; ++previous) {
                            const sdk::format::DialogueCueText& prior = dialogueRows[previous];
                            if (prior.slotIndex == slotRow && prior.cueIndex == cue
                                && catalog.string(prior.text) == candidate) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (duplicate) {
                            continue;
                        }
                        hasCandidate = true;
                        ImGui::BulletText("%.*s", print_length(candidate), candidate.data());
                    }
                    if (!hasCandidate) {
                        ImGui::TextDisabled("No translations found.");
                    }
                    ImGui::Unindent();
                    if (available != mission::SceneStatus::ready) {
                        ImGui::TextDisabled("%s", mission::status_name(available));
                    }
                    if (g_hasDialogueActionStatus && g_dialogueActionOccurrence == occurrenceRow
                        && g_dialogueActionSlot == slotRow && g_dialogueActionCue == cue) {
                        ImGui::TextDisabled("last: %s",
                                            mission::status_name(g_dialogueActionStatus));
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    if (sensorCount == 0) {
        ImGui::TextDisabled("No cue matches.");
    }
}

/** Draws exact generated type-68 HUD elements with their native state transitions. */
void draw_directives(const sdk::BoundView& view, const mission::Snapshot& snapshot) noexcept {
    if (view.catalog == nullptr) {
        return;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto occurrences = catalog.occurrences();
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    const std::string_view query = search_text();
    std::size_t rows = 0;
    for (std::uint32_t occurrenceRow = 0; occurrenceRow < occurrences.size(); ++occurrenceRow) {
        const sdk::format::Occurrence& occurrence = occurrences[occurrenceRow];
        if (occurrence.scenarioIndex != view.scenarioRow
            || occurrence.stateIndex != snapshot.plan.stateRow
            || occurrence.objectIndex >= objects.size()) {
            continue;
        }
        for (const sdk::format::DirectiveElement& directive : catalog.directive_elements()) {
            if (directive.slotIndex >= slots.size()
                || slots[directive.slotIndex].objectIndex != occurrence.objectIndex) {
                continue;
            }
            const std::string_view title = display_text(catalog, directive.title);
            const std::string_view description = display_text(catalog, directive.description);
            const std::string_view progress = display_text(catalog, directive.progress);
            const std::string_view id = display_text(catalog, directive.id);
            if (!query.empty() && !contains_folded(title, query)
                && !contains_folded(description, query) && !contains_folded(progress, query)
                && !contains_folded(id, query)) {
                continue;
            }
            ++rows;
            const mission::SceneStatus available =
                mission::directive_availability(view,
                                                occurrenceRow,
                                                directive.slotIndex,
                                                directive.nameHash,
                                                directive.elementIndex);
            ImGui::PushID(static_cast<int>(occurrenceRow));
            ImGui::PushID(static_cast<int>(directive.slotIndex));
            ImGui::PushID(static_cast<int>(directive.nameHash));
            ImGui::PushID(directive.elementIndex);
            ImGui::Text("%.*s", print_length(title), title.data());
            if (!description.empty()) {
                ImGui::TextWrapped("%.*s", print_length(description), description.data());
            }
            if (!progress.empty()) {
                ImGui::TextWrapped("%.*s%s",
                                   print_length(progress),
                                   progress.data(),
                                   (directive.flags & sdk::format::kDirectiveElementCounter) != 0
                                       ? " (counter)"
                                       : "");
            }
            ImGui::TextDisabled("hash %08X  element %d  slot %u",
                                static_cast<unsigned>(directive.nameHash),
                                directive.elementIndex,
                                static_cast<unsigned>(directive.slotIndex));
            ImGui::BeginDisabled(available != mission::SceneStatus::ready);
            const auto apply = [&](const char* label, std::int8_t state, bool visible) {
                if (ImGui::Button(label)) {
                    g_directiveActionOccurrence = occurrenceRow;
                    g_directiveActionSlot = directive.slotIndex;
                    g_directiveActionHash = directive.nameHash;
                    g_directiveActionElement = directive.elementIndex;
                    g_directiveActionStatus = mission::set_directive(view,
                                                                     occurrenceRow,
                                                                     directive.slotIndex,
                                                                     directive.nameHash,
                                                                     directive.elementIndex,
                                                                     state,
                                                                     visible);
                    g_hasDirectiveActionStatus = true;
                }
            };
            apply("Show", 0, true);
            ImGui::SameLine();
            apply("Complete", 1, true);
            ImGui::SameLine();
            apply("Alternate exit", 2, true);
            ImGui::SameLine();
            apply("Hide", 0, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mission::status_name(available));
            if (g_hasDirectiveActionStatus && g_directiveActionOccurrence == occurrenceRow
                && g_directiveActionSlot == directive.slotIndex
                && g_directiveActionHash == directive.nameHash
                && g_directiveActionElement == directive.elementIndex) {
                ImGui::TextDisabled("last: %s", mission::status_name(g_directiveActionStatus));
            }
            ImGui::Separator();
            ImGui::PopID();
            ImGui::PopID();
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    if (rows == 0) {
        ImGui::TextDisabled("No directive matches.");
    }
}

/** Keeps action feedback tied to the exact generation that produced it. */
void sync_action_generation(const sdk::BoundView& view,
                            const mission::Snapshot& snapshot) noexcept {
    if (g_actionGeneration == snapshot.activityClientGeneration
        && g_actionScenario == view.scenarioRow) {
        return;
    }
    g_actionGeneration = snapshot.activityClientGeneration;
    g_actionScenario = view.scenarioRow;
    g_hasSceneActionStatus = false;
    g_hasDialogueActionStatus = false;
    reset_behavior_action_status();
    g_hasDirectiveActionStatus = false;
}
} // namespace
/** @return A generated string or a stable missing marker. */
[[nodiscard]] std::string_view display_text(const sdk::Catalog& catalog,
                                            sdk::format::StringRef value) noexcept {
    const std::string_view text = catalog.string(value);
    return text.empty() ? std::string_view("-") : text;
}

/** @return A string length bounded for printf-style rendering. */
[[nodiscard]] int print_length(std::string_view value) noexcept {
    return static_cast<int>((std::min)(value.size(), static_cast<std::size_t>(INT_MAX)));
}

/** ASCII case-insensitive substring match for stable generated IDs and numeric search text. */
[[nodiscard]] bool contains_folded(std::string_view value, std::string_view query) noexcept {
    if (query.empty()) {
        return true;
    }
    if (query.size() > value.size()) {
        return false;
    }
    for (std::size_t start = 0; start <= value.size() - query.size(); ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < query.size(); ++index) {
            const auto left = static_cast<unsigned char>(value[start + index]);
            const auto right = static_cast<unsigned char>(query[index]);
            equal = equal && std::tolower(left) == std::tolower(right);
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

/** @return Search text without trailing zero storage. */
[[nodiscard]] std::string_view search_text() noexcept {
    std::size_t length = 0;
    while (length < g_search.size() && g_search[length] != '\0') {
        ++length;
    }
    return {g_search.data(), length};
}

/** Draws the automatically selected state-0 roster plan. */
void draw(const sdk::BoundView& view, const sdk::format::Scenario& scenario) noexcept {
    mission::Snapshot snapshot{};
    const mission::Status available = mission::query(view, snapshot);
    sync_action_generation(view, snapshot);
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("Roster view unavailable.");
        return;
    }
    static_cast<void>(scenario);

    section::header("Current connection", nullptr);
    ImGui::TextWrapped("The server picked this object and schema roster. The destination and the "
                       "arrival slice choose them.");
    ImGui::Text("Availability  %s", mission::status_name(available));
    ImGui::Text("Publication  %s%s",
                snapshot.configured ? "selected" : "not selected",
                snapshot.publicationPending ? "  publication pending" : "");
    if (ImGui::TreeNodeEx("Technical details##mission_connection",
                          ImGuiTreeNodeFlags_SpanAvailWidth)) {
        const auto& plan = snapshot.plan;
        ImGui::Text("ActivityClient generation %llu  arrival slice %d  D6 slice %d",
                    static_cast<unsigned long long>(snapshot.activityClientGeneration),
                    snapshot.arrivalSliceSetIndex,
                    snapshot.liveSliceSetIndex);
        ImGui::Text("activity row %u  scenario row %u", plan.activityRow, plan.scenarioRow);
        ImGui::Text("state row %u  ordinal %u  entry %u  slice %u",
                    plan.stateRow,
                    plan.stateOrdinal,
                    plan.entryIndex,
                    plan.sliceSetIndex);
        ImGui::Text("bubble row %u  ordinal %u", plan.bubbleRow, plan.bubbleOrdinal);
        ImGui::Text("occurrences %u  groups %u  Auth mapping slots %u",
                    plan.occurrenceCount,
                    plan.groupCount,
                    plan.authMappingSlots);
        ImGui::Text("Auth reset slots %u  Sense-suppressed slots %u",
                    plan.authResetSlots,
                    plan.senseSuppressedSlots);
        ImGui::Text("revision %llu / published %llu",
                    static_cast<unsigned long long>(snapshot.revision),
                    static_cast<unsigned long long>(snapshot.publishedRevision));
        ImGui::TreePop();
    }
}

/**
 * Draws the one search box every page here shares and reads the current mission state.
 * @param snapshot Cleared, then receives the queried state.
 * @return True when a page can draw its rows.
 */
[[nodiscard]] static bool page_prologue(const sdk::BoundView& view,
                                        mission::Snapshot& snapshot) noexcept {
    snapshot = {};
    (void)mission::query(view, snapshot);
    sync_action_generation(view, snapshot);
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return false;
    }
    ImGui::InputTextWithHint(
        "Search##sdk_mission_seed", "name or id", g_search.data(), g_search.size());
    return true;
}

void draw_scenes(const sdk::BoundView& view) noexcept {
    mission::Snapshot snapshot{};
    if (page_prologue(view, snapshot)) {
        draw_authored_scenes(view, snapshot);
    }
}

void draw_dialogue(const sdk::BoundView& view) noexcept {
    mission::Snapshot snapshot{};
    if (page_prologue(view, snapshot)) {
        draw_dialogues(view, snapshot);
    }
}

void draw_directives(const sdk::BoundView& view) noexcept {
    mission::Snapshot snapshot{};
    if (page_prologue(view, snapshot)) {
        draw_directives(view, snapshot);
    }
}

void draw_objectives(const sdk::BoundView& view) noexcept {
    mission::Snapshot snapshot{};
    if (page_prologue(view, snapshot)) {
        draw_behavior_inventory(view, snapshot, BehaviorFamily::objectives);
    }
}

void draw_cinematics(const sdk::BoundView& view) noexcept {
    mission::Snapshot snapshot{};
    if (page_prologue(view, snapshot)) {
        draw_behavior_inventory(view, snapshot, BehaviorFamily::cinematics);
    }
}

void draw_compiled_behaviors(const sdk::BoundView& view) noexcept {
    mission::Snapshot snapshot{};
    if (page_prologue(view, snapshot)) {
        draw_compiled_behavior_roots(*view.catalog);
    }
}

} // namespace sunrise::server::ui::activity_host::sdk_mission_view
