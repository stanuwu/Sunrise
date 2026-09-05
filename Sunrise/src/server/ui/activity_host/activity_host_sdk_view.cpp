#include "activity_host_sdk_view.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../client/ui/activity/authored_placement_marker.h"
#include "../../../core/ui/components/card/ui_card_component.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../state/activity_sdk/generation/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../activity/activity_sdk_device_runtime.h"
#include "../../activity/host_runtime.h"
#include "../../activity/mission/mission_script_runtime.h"
#include "../../bap/runtime.h"
#include "activity_host_scriptable_browser.h"
#include "activity_host_sdk_mission_view.h"
#include "activity_host_sdk_squad_view.h"
#include "activity_host_sdk_state_pages.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::sdk_view {
namespace {

namespace card = core::ui::components::card;
namespace format = state::activity_sdk::format;
namespace generation = state::activity_sdk::generation;
namespace host = server::activity::host;
namespace marker = client::ui::activity::authored_placement_marker;
namespace mission = server::activity::mission;
namespace sdk = state::activity_sdk;
namespace devices = server::activity::activity_sdk_devices;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace scaling = core::ui::scaling::dpi;

/** Width of the navigation column, which fits the longest page name with room to spare. */
constexpr float kNavigationWidth = 132.0F;

constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
constexpr ImGuiTableFlags kWideTableFlags =
    kTableFlags | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable;

std::weak_ptr<const sdk::Catalog> g_selectionCatalog{};
state::activity::SessionBinding g_selectionBinding{};
std::uint64_t g_selectionClientGeneration{};
std::uint32_t g_selectedOccurrence{format::kAbsentIndex};
std::uint32_t g_selectedSlot{format::kAbsentIndex};
std::uint32_t g_deviceSlot{format::kAbsentIndex};
int g_deviceChannel{};
float g_deviceValue{1.0F};
bool g_deviceSnap{};
bool g_hasDeviceResult{};
devices::Status g_deviceResult{devices::Status::invalidSlot};
std::uint32_t g_triggerSlot{format::kAbsentIndex};
bool g_hasTriggerResult{};
devices::Status g_triggerResult{devices::Status::invalidSlot};
std::array<char, 256> g_generatedSymbolSearch{};
std::array<char, 256> g_topologySearch{};
std::vector<std::uint32_t> g_filteredRows{};
bool g_scriptReloadRequested{};
bool g_scriptReloadAccepted{};

/** @return A catalog row count bounded for Dear ImGui's signed clipper. */
[[nodiscard]] int clipped_count(std::size_t count) noexcept {
    return static_cast<int>((std::min)(count, static_cast<std::size_t>(INT_MAX)));
}

/** Draws the selected activity's Lua attach, VM, delivery, and Host-lane health. */
void draw_script_runtime(const host::InstanceSnapshot& hostInstance) noexcept {
    mission::DiagnosticsSnapshot runtime{};
    mission::snapshot(runtime);
    ImGui::Text("Runtime %s", runtime.enabled ? "on" : "off");
    ImGui::SameLine();
    ImGui::TextDisabled("folder %s", runtime.pathReady ? "ready" : "unavailable");

    std::array<char, 260> controller{};
    const mission::InstanceDiagnostics* selected = nullptr;
    for (std::size_t index = 0; index < runtime.instanceCount; ++index) {
        if (same_binding(runtime.instances[index].binding, hostInstance.binding)) {
            selected = &runtime.instances[index];
            break;
        }
    }
    const mission::AttachDiagnostics* attach = nullptr;
    for (std::size_t index = 0; index < runtime.attachCount; ++index) {
        if (same_binding(runtime.attaches[index].binding, hostInstance.binding)) {
            attach = &runtime.attaches[index];
            break;
        }
    }
    const std::uint32_t activityRow = selected != nullptr ? selected->activityRow
                                      : attach != nullptr ? attach->activityRow
                                                          : 0;
    if (activityRow != 0 && mission::controller_file_name(activityRow, controller)) {
        ImGui::Text("File Sunrise/scripts/%s", controller.data());
    }
    if (attach != nullptr) {
        ImGui::Text("Attach %s", attach->result.data());
        if (attach->detail[0] != '\0') {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", attach->detail.data());
        }
    } else {
        ImGui::TextDisabled("No attach result yet");
    }

    ImGui::SeparatorText("VM");
    if (selected == nullptr) {
        ImGui::TextDisabled("No mission VM instance");
    } else {
        ImGui::Text("Program %s", selected->programStatus.data());
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s",
                            selected->vmActive ? "active" : "inactive",
                            selected->vmFaulted ? " faulted" : "");
        ImGui::Text("Last %s: %s", selected->lastVmStage.data(), selected->lastVmStatus.data());
        if (selected->lastVmError[0] != '\0') {
            ImGui::TextWrapped("Error %s", selected->lastVmError.data());
        }
        ImGui::Text("Callbacks %llu committed %llu refused %llu",
                    static_cast<unsigned long long>(selected->vmCallbacks),
                    static_cast<unsigned long long>(selected->vmCommittedCallbacks),
                    static_cast<unsigned long long>(selected->vmRefusedCallbacks));
        ImGui::Text("Events %llu/%llu pending %zu",
                    static_cast<unsigned long long>(selected->eventsCommitted),
                    static_cast<unsigned long long>(selected->eventsSeen),
                    selected->pendingEvents);
        ImGui::Text("Delivery %s intents %zu attempts %u",
                    selected->deliveryStage.data(),
                    selected->pendingIntents,
                    selected->intentAttempts);
    }

    ImGui::SeparatorText("Host output lane");
    ImGui::Text("Status %s", host::output_status_name(hostInstance.outputStatus));
    ImGui::SameLine();
    ImGui::TextDisabled("kind %u attempts %u",
                        static_cast<unsigned>(hostInstance.outputKind),
                        hostInstance.outputAttempts);
    ImGui::Text("Queued %s", hostInstance.outputPending ? "yes" : "no");

    if (ImGui::Button("Reload script")) {
        g_scriptReloadRequested = true;
        g_scriptReloadAccepted = mission::reload();
    }
    if (g_scriptReloadRequested) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_scriptReloadAccepted ? "reload queued" : "runtime unavailable");
    }
}

/** @return Non-null display storage for one optional generated string. */
[[nodiscard]] const char* display_data(std::string_view value) noexcept {
    return value.empty() ? "-" : value.data();
}

/** @return A generated string length bounded for printf-style presentation. */
[[nodiscard]] int display_length(std::string_view value) noexcept {
    return value.empty() ? 1 : clipped_count(value.size());
}

/** Draws a generated string or a stable missing marker. */
void draw_string(const sdk::Catalog& catalog, format::StringRef reference) noexcept {
    const std::string_view value = catalog.string(reference);
    if (value.empty()) {
        ImGui::TextDisabled("-");
        return;
    }
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

/** @return A generated string when one global row index is valid. */
template <typename Row>
[[nodiscard]] std::string_view
row_id(const sdk::Catalog& catalog, std::span<const Row> rows, std::uint32_t index) noexcept {
    return index < rows.size() ? catalog.string(rows[index].id) : std::string_view{};
}

/** Draws one generated string inline after a fixed label. */
void draw_labeled_string(const char* label,
                         const sdk::Catalog& catalog,
                         format::StringRef reference) noexcept {
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    draw_string(catalog, reference);
}

/** Formats one SHA-256 identity without allocating panel state. */
void draw_digest(const char* label, std::span<const std::byte> digest) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 65> text{};
    if (digest.size() != 32) {
        ImGui::Text("%s  invalid", label);
        return;
    }
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const unsigned value = std::to_integer<unsigned>(digest[index]);
        text[index * 2] = digits[(value >> 4U) & 0xFU];
        text[index * 2 + 1] = digits[value & 0xFU];
    }
    ImGui::Text("%s  %s", label, text.data());
}

/** Formats the stable inspect, panel-test, and script exposure mask. */
void format_exposures(std::uint32_t flags, std::array<char, 96>& output) noexcept {
    if (flags == 0) {
        (void)std::snprintf(output.data(), output.size(), "none");
        return;
    }
    const std::uint32_t unknown = flags & ~format::kExposureMask;
    (void)std::snprintf(output.data(),
                        output.size(),
                        "%s%s%s%s0x%X",
                        (flags & format::kInspectExposure) != 0 ? "inspect " : "",
                        (flags & format::kPanelTestExposure) != 0 ? "panel-test " : "",
                        (flags & format::kScriptExposure) != 0 ? "script " : "",
                        unknown != 0 ? "unknown " : "mask ",
                        static_cast<unsigned>(flags));
}

/** @return Stable text for one generated alias kind. */
[[nodiscard]] const char* text_kind_name(std::uint32_t kind) noexcept {
    switch (static_cast<format::TextKind>(kind)) {
    case format::TextKind::internalAlias:
        return "internal";
    case format::TextKind::displayAlias:
        return "display";
    case format::TextKind::slotAlias:
        return "slot";
    case format::TextKind::refusalReason:
        return "refusal";
    }
    return "unknown";
}

/** @return Stable text for one generated capability subject. */
[[nodiscard]] const char* subject_kind_name(std::uint32_t kind) noexcept {
    switch (static_cast<format::SubjectKind>(kind)) {
    case format::SubjectKind::activity:
        return "activity";
    case format::SubjectKind::slot:
        return "slot";
    case format::SubjectKind::hostApi:
        return "host API";
    }
    return "unknown";
}

/** @return True for ASCII whitespace accepted around one panel search. */
[[nodiscard]] constexpr bool search_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

/** @return One trimmed, null-terminated panel search buffer. */
template <std::size_t Size>
[[nodiscard]] std::string_view search_text(const std::array<char, Size>& buffer) noexcept {
    std::string_view value(buffer.data());
    while (!value.empty() && search_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && search_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

/** One case-insensitive generated-row search with decimal and hexadecimal numeric matching. */
struct SearchQuery final {
    std::string_view value{};

    /** @return True when the user has not entered a searchable value. */
    [[nodiscard]] bool empty() const noexcept {
        return value.empty();
    }

    /** @return True when one generated string contains this search. */
    [[nodiscard]] bool matches(std::string_view candidate) const noexcept {
        if (empty()) {
            return true;
        }
        if (candidate.size() < value.size()) {
            return false;
        }
        const auto fold = [](char character) noexcept {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                        : character;
        };
        for (std::size_t offset = 0; offset + value.size() <= candidate.size(); ++offset) {
            bool equal = true;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (fold(candidate[offset + index]) != fold(value[index])) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                return true;
            }
        }
        return false;
    }

    /** @return True when one generated string reference contains this search. */
    [[nodiscard]] bool matches(const sdk::Catalog& catalog,
                               format::StringRef reference) const noexcept {
        return matches(catalog.string(reference));
    }

    /** @return True when decimal, compact hex, padded hex, or its field label matches. */
    [[nodiscard]] bool matches(std::string_view label, std::uint32_t number) const noexcept {
        std::array<char, 96> text{};
        (void)std::snprintf(text.data(),
                            text.size(),
                            "%.*s %u 0x%X 0x%08X",
                            clipped_count(label.size()),
                            label.data(),
                            static_cast<unsigned>(number),
                            static_cast<unsigned>(number),
                            static_cast<unsigned>(number));
        return matches(std::string_view(text.data()));
    }
};

/** Draws one stable search box shared by a generated SDK collection. */
template <std::size_t Size>
void draw_search(std::array<char, Size>& buffer, const char* id, const char* hint) noexcept {
    ImGui::SetNextItemWidth(420.0F);
    (void)ImGui::InputTextWithHint(id, hint, buffer.data(), buffer.size());
}

/** @return True when one package alias matches the current search. */
[[nodiscard]] bool aliases_match(const sdk::Catalog& catalog,
                                 const SearchQuery& query,
                                 std::span<const format::Text> aliases) noexcept {
    for (const format::Text& alias : aliases) {
        if (query.matches(catalog, alias.value) || query.matches(text_kind_name(alias.kind))) {
            return true;
        }
    }
    return false;
}

/** Keeps object and slot selection pinned to one catalog and client generation. */
void sync_selection(const sdk::BoundView& view,
                    std::span<const format::Occurrence> occurrences) noexcept {
    if (g_selectionCatalog.lock() != view.catalog || !same_binding(g_selectionBinding, view.binding)
        || g_selectionClientGeneration != view.activityClientGeneration) {
        g_selectionCatalog = view.catalog;
        g_selectionBinding = view.binding;
        g_selectionClientGeneration = view.activityClientGeneration;
        g_selectedOccurrence = occurrences.empty() ? format::kAbsentIndex : 0;
        g_selectedSlot = format::kAbsentIndex;
        g_deviceSlot = format::kAbsentIndex;
        g_hasDeviceResult = false;
    }
    if (g_selectedOccurrence >= occurrences.size()) {
        g_selectedOccurrence = occurrences.empty() ? format::kAbsentIndex : 0;
        g_selectedSlot = format::kAbsentIndex;
    }
}

/** Draws the selected activity's names first, and its pinned identities on demand. */
void draw_identity(const sdk::BoundView& view,
                   const format::Activity& activity,
                   const format::Scenario& scenario) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    draw_labeled_string("Activity", catalog, activity.displayName);
    draw_labeled_string("Package", catalog, activity.internalName);
    draw_labeled_string("Scenario", catalog, scenario.name);
    if (!ImGui::TreeNodeEx("Technical details##activity_identity",
                           ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    draw_labeled_string("Activity ID", catalog, activity.id);
    draw_labeled_string("Scenario ID", catalog, scenario.id);
    ImGui::Text("activity row %u  index %u  definition 0x%08X  flags 0x%08X",
                static_cast<unsigned>(view.activityRow),
                static_cast<unsigned>(activity.activityIndex),
                static_cast<unsigned>(activity.definitionHash),
                static_cast<unsigned>(activity.flags));
    ImGui::Text("scenario row %u  tag 0x%08X",
                static_cast<unsigned>(view.scenarioRow),
                static_cast<unsigned>(scenario.tag));
    ImGui::Text("session 0x%llX  revision %llu  ActivityClient generation %llu",
                static_cast<unsigned long long>(view.binding.sessionId),
                static_cast<unsigned long long>(view.binding.createdRevision),
                static_cast<unsigned long long>(view.activityClientGeneration));
    draw_digest("SDK build", catalog.sdk_build_sha256());
    draw_digest("Content key", catalog.content_key_sha256());
    draw_digest("Logical IR", catalog.logical_ir_sha256());
    ImGui::TreePop();
}

/** Draws generated internal and display aliases. */
void draw_aliases(const sdk::Catalog& catalog,
                  std::span<const format::Text> aliases,
                  const char* tableId) noexcept {
    if (aliases.empty()) {
        ImGui::TextDisabled("No aliases");
        return;
    }
    if (!ImGui::BeginTable(tableId, 2, kTableFlags, table_layout::size(aliases.size()))) {
        return;
    }
    ImGui::TableSetupColumn("kind");
    ImGui::TableSetupColumn("alias", ImGuiTableColumnFlags_WidthStretch);
    table_layout::frozen_headers();
    for (const format::Text& alias : aliases) {
        table_layout::next_row();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(text_kind_name(alias.kind));
        ImGui::TableNextColumn();
        draw_string(catalog, alias.value);
    }
    ImGui::EndTable();
}

/** @return True when one scenario identity or package tag matches. */
[[nodiscard]] bool scenario_matches(const sdk::Catalog& catalog,
                                    const SearchQuery& query,
                                    const format::Scenario& scenario) noexcept {
    return query.matches(catalog, scenario.id) || query.matches(catalog, scenario.name)
           || query.matches("scenario tag", scenario.tag);
}

/** @return True when one bubble ID, name, hash, ordinal, or row identity matches. */
[[nodiscard]] bool bubble_matches(const sdk::Catalog& catalog,
                                  const SearchQuery& query,
                                  const format::Bubble& bubble) noexcept {
    return query.matches(catalog, bubble.id) || query.matches(catalog, bubble.name)
           || query.matches("scenario index", bubble.scenarioIndex)
           || query.matches("bubble ordinal", bubble.bubbleOrdinal)
           || query.matches("bubble hash", bubble.nameHash);
}

/** @return True when one state ID, hash, ordinal, public value, or row identity matches. */
[[nodiscard]] bool state_matches(const sdk::Catalog& catalog,
                                 const SearchQuery& query,
                                 const format::State& state) noexcept {
    return query.matches(catalog, state.id) || query.matches(catalog, state.entryId)
           || query.matches(catalog, state.registryId)
           || query.matches("scenario index", state.scenarioIndex)
           || query.matches("bubble index", state.bubbleIndex)
           || query.matches("state ordinal", state.stateOrdinal)
           || query.matches("map bubble index", state.mapBubbleIndex)
           || query.matches("state hash", state.stateHash)
           || query.matches("public value", state.publicValue)
           || query.matches("state flags", state.flags)
           || query.matches("registry tag", state.registryTag);
}

/** @return True when a bubble, its scenario, or any owned state matches. */
[[nodiscard]] bool topology_bubble_matches(const sdk::Catalog& catalog,
                                           const SearchQuery& query,
                                           const format::Scenario& scenario,
                                           const format::Bubble& bubble) noexcept {
    if (scenario_matches(catalog, query, scenario) || bubble_matches(catalog, query, bubble)) {
        return true;
    }
    for (const format::State& state : sdk::bubble_states(catalog, bubble)) {
        if (state_matches(catalog, query, state)) {
            return true;
        }
    }
    return false;
}

/** @return True when a state or its exact scenario/bubble context matches. */
[[nodiscard]] bool topology_state_matches(const sdk::Catalog& catalog,
                                          const SearchQuery& query,
                                          const format::Scenario& scenario,
                                          const format::State& state) noexcept {
    if (scenario_matches(catalog, query, scenario) || state_matches(catalog, query, state)) {
        return true;
    }
    const auto bubbles = catalog.bubbles();
    return state.bubbleIndex < bubbles.size()
           && bubble_matches(catalog, query, bubbles[state.bubbleIndex]);
}

/** Draws the bubbles and their generated state slices. */
void draw_bubbles(const sdk::Catalog& catalog,
                  const format::Scenario& scenario,
                  const SearchQuery& query) noexcept {
    const auto bubbles = sdk::scenario_bubbles(catalog, scenario);
    g_filteredRows.clear();
    g_filteredRows.reserve(bubbles.size());
    for (std::size_t index = 0; index < bubbles.size(); ++index) {
        if (topology_bubble_matches(catalog, query, scenario, bubbles[index])) {
            g_filteredRows.push_back(static_cast<std::uint32_t>(index));
        }
    }
    ImGui::Text("%zu of %zu bubbles", g_filteredRows.size(), bubbles.size());
    if (!ImGui::BeginTable(
            "##sdk_bubbles", 5, kTableFlags, table_layout::size(g_filteredRows.size()))) {
        return;
    }
    ImGui::TableSetupColumn("ordinal");
    ImGui::TableSetupColumn("hash");
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("states");
    table_layout::frozen_headers();
    for (const std::uint32_t index : g_filteredRows) {
        const format::Bubble& bubble = bubbles[index];
        table_layout::next_row();
        ImGui::TableNextColumn();
        ImGui::Text("%u", static_cast<unsigned>(bubble.bubbleOrdinal));
        ImGui::TableNextColumn();
        ImGui::Text("0x%08X", static_cast<unsigned>(bubble.nameHash));
        ImGui::TableNextColumn();
        draw_string(catalog, bubble.name);
        ImGui::TableNextColumn();
        draw_string(catalog, bubble.id);
        ImGui::TableNextColumn();
        ImGui::Text("%zu", sdk::bubble_states(catalog, bubble).size());
    }
    ImGui::EndTable();
}

/** Draws all exact state identities in the scenario. */
void draw_states(const sdk::Catalog& catalog,
                 const format::Scenario& scenario,
                 const SearchQuery& query) noexcept {
    const auto states = sdk::scenario_states(catalog, scenario);
    g_filteredRows.clear();
    g_filteredRows.reserve(states.size());
    for (std::size_t index = 0; index < states.size(); ++index) {
        if (topology_state_matches(catalog, query, scenario, states[index])) {
            g_filteredRows.push_back(static_cast<std::uint32_t>(index));
        }
    }
    ImGui::Text("%zu of %zu states", g_filteredRows.size(), states.size());
    if (!ImGui::BeginTable(
            "##sdk_states", 9, kWideTableFlags, table_layout::size(g_filteredRows.size()))) {
        return;
    }
    ImGui::TableSetupColumn("ordinal");
    ImGui::TableSetupColumn("bubble row");
    ImGui::TableSetupColumn("hash");
    ImGui::TableSetupColumn("public");
    ImGui::TableSetupColumn("flags");
    ImGui::TableSetupColumn("map bubble");
    ImGui::TableSetupColumn("ID");
    ImGui::TableSetupColumn("entry ID");
    ImGui::TableSetupColumn("registry ID");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(clipped_count(g_filteredRows.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::size_t index = g_filteredRows[static_cast<std::size_t>(row)];
            const format::State& state = states[index];
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(state.stateOrdinal));
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(state.bubbleIndex));
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(state.stateHash));
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(state.publicValue));
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(state.flags));
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(state.mapBubbleIndex));
            ImGui::TableNextColumn();
            draw_string(catalog, state.id);
            ImGui::TableNextColumn();
            draw_string(catalog, state.entryId);
            ImGui::TableNextColumn();
            draw_string(catalog, state.registryId);
        }
    }
    ImGui::EndTable();
}

/** @return True when one exact slot identity, schema, alias, or numeric field matches. */
[[nodiscard]] bool slot_matches(const sdk::Catalog& catalog,
                                const SearchQuery& query,
                                const format::Slot& slot) noexcept {
    return query.matches(catalog, slot.id) || query.matches(catalog, slot.name)
           || query.matches(catalog, slot.senseSchemaId)
           || query.matches(catalog, slot.authSchemaId)
           || query.matches("object index", slot.objectIndex)
           || query.matches("slot index", slot.slotIndex)
           || query.matches("slot type", slot.slotType)
           || query.matches("component class", slot.componentClass)
           || query.matches("sense schema", slot.senseSchema)
           || query.matches("auth schema", slot.authSchema)
           || query.matches("slot flags", slot.flags)
           || aliases_match(catalog, query, sdk::slot_aliases(catalog, slot));
}

/** @return True when one generated object identity, tag, key, or count matches. */
[[nodiscard]] bool object_matches(const sdk::Catalog& catalog,
                                  const SearchQuery& query,
                                  const format::Object& object) noexcept {
    return query.matches(catalog, object.id) || query.matches("object tag", object.objectTag)
           || query.matches("object key", object.objectKey)
           || query.matches("config count", object.configCount)
           || query.matches("descriptor count", object.descriptorCount)
           || query.matches("placed subblock count", object.placedSubblockCount)
           || query.matches("placed leaf count", object.placedLeafCount)
           || query.matches("placed hop count", object.placedHopCount)
           || query.matches("bare target count", object.bareTargetCount);
}

/** @return True when one occurrence's own registry and topology context matches. */
[[nodiscard]] bool occurrence_identity_matches(const sdk::Catalog& catalog,
                                               const SearchQuery& query,
                                               const format::Occurrence& occurrence) noexcept {
    return query.matches(catalog, occurrence.id)
           || query.matches(catalog, occurrence.contextRegistryKey)
           || query.matches(catalog, occurrence.registryId)
           || query.matches(catalog, occurrence.entryId)
           || query.matches("scenario index", occurrence.scenarioIndex)
           || query.matches("bubble index", occurrence.bubbleIndex)
           || query.matches("state index", occurrence.stateIndex)
           || query.matches("object index", occurrence.objectIndex)
           || query.matches("registry field", occurrence.registryField)
           || query.matches("object ordinal", occurrence.objectOrdinal)
           || query.matches(row_id(catalog, catalog.bubbles(), occurrence.bubbleIndex))
           || query.matches(row_id(catalog, catalog.states(), occurrence.stateIndex));
}

/** @return True when an occurrence, its object, or any reusable slot matches. */
[[nodiscard]] bool occurrence_matches(const sdk::Catalog& catalog,
                                      const SearchQuery& query,
                                      const format::Occurrence& occurrence) noexcept {
    if (query.empty() || occurrence_identity_matches(catalog, query, occurrence)) {
        return true;
    }
    const auto objects = catalog.objects();
    if (occurrence.objectIndex >= objects.size()) {
        return false;
    }
    const format::Object& object = objects[occurrence.objectIndex];
    if (object_matches(catalog, query, object)) {
        return true;
    }
    for (const format::Slot& slot : sdk::object_slots(catalog, object)) {
        if (slot_matches(catalog, query, slot)) {
            return true;
        }
    }
    return false;
}

/** @return True when a selected-object slot or its occurrence/object context matches. */
[[nodiscard]] bool selected_slot_matches(const sdk::Catalog& catalog,
                                         const SearchQuery& query,
                                         const format::Occurrence& occurrence,
                                         const format::Object& object,
                                         const format::Slot& slot) noexcept {
    return query.empty() || occurrence_identity_matches(catalog, query, occurrence)
           || object_matches(catalog, query, object) || slot_matches(catalog, query, slot);
}

/** Draws the scenario occurrence symbols and updates the selected object. */
void draw_occurrences(const sdk::Catalog& catalog,
                      std::span<const format::Occurrence> occurrences,
                      const SearchQuery& query) noexcept {
    const auto objects = catalog.objects();
    const auto bubbles = catalog.bubbles();
    const auto states = catalog.states();
    g_filteredRows.clear();
    g_filteredRows.reserve(occurrences.size());
    for (std::size_t index = 0; index < occurrences.size(); ++index) {
        if (occurrence_matches(catalog, query, occurrences[index])) {
            g_filteredRows.push_back(static_cast<std::uint32_t>(index));
        }
    }
    ImGui::Text("%zu of %zu objects", g_filteredRows.size(), occurrences.size());
    if (!ImGui::BeginTable(
            "##sdk_occurrences", 8, kWideTableFlags, table_layout::size(g_filteredRows.size()))) {
        return;
    }
    ImGui::TableSetupColumn("occurrence", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("object", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("context registry", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("registry ID");
    ImGui::TableSetupColumn("entry ID");
    ImGui::TableSetupColumn("field");
    ImGui::TableSetupColumn("bubble");
    ImGui::TableSetupColumn("state");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(clipped_count(g_filteredRows.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::size_t index = g_filteredRows[static_cast<std::size_t>(row)];
            const format::Occurrence& occurrence = occurrences[index];
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(index));
            const std::string_view id = catalog.string(occurrence.id);
            std::array<char, 160> label{};
            (void)std::snprintf(
                label.data(), label.size(), "%.*s", display_length(id), display_data(id));
            if (table_layout::selectable(
                    label.data(), g_selectedOccurrence == static_cast<std::uint32_t>(index))) {
                g_selectedOccurrence = static_cast<std::uint32_t>(index);
                g_selectedSlot = format::kAbsentIndex;
                g_deviceSlot = format::kAbsentIndex;
                g_hasDeviceResult = false;
            }
            ImGui::TableNextColumn();
            const std::string_view objectId = row_id(catalog, objects, occurrence.objectIndex);
            if (objectId.empty()) {
                ImGui::TextDisabled("invalid row %u",
                                    static_cast<unsigned>(occurrence.objectIndex));
            } else {
                ImGui::TextUnformatted(objectId.data(), objectId.data() + objectId.size());
            }
            ImGui::TableNextColumn();
            draw_string(catalog, occurrence.contextRegistryKey);
            ImGui::TableNextColumn();
            draw_string(catalog, occurrence.registryId);
            ImGui::TableNextColumn();
            draw_string(catalog, occurrence.entryId);
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(occurrence.registryField));
            ImGui::TableNextColumn();
            const std::string_view bubbleId = row_id(catalog, bubbles, occurrence.bubbleIndex);
            if (bubbleId.empty()) {
                ImGui::Text("row %u", static_cast<unsigned>(occurrence.bubbleIndex));
            } else {
                ImGui::TextUnformatted(bubbleId.data(), bubbleId.data() + bubbleId.size());
            }
            ImGui::TableNextColumn();
            const std::string_view stateId = row_id(catalog, states, occurrence.stateIndex);
            if (stateId.empty()) {
                ImGui::Text("row %u", static_cast<unsigned>(occurrence.stateIndex));
            } else {
                ImGui::TextUnformatted(stateId.data(), stateId.data() + stateId.size());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

/** Draws package aliases attached to one object slot. */
void draw_slot_aliases(const sdk::Catalog& catalog, const format::Slot& slot) noexcept {
    const auto aliases = sdk::slot_aliases(catalog, slot);
    ImGui::Text("%zu alias%s", aliases.size(), aliases.size() == 1 ? "" : "es");
    for (const format::Text& alias : aliases) {
        ImGui::Bullet();
        ImGui::SameLine();
        draw_string(catalog, alias.value);
    }
}

/** Draws the exact generated SDK type-23 control used by Lua `SlotView:set_channel`. */
void draw_device_action(const sdk::BoundView& view,
                        const format::Slot& slot,
                        std::uint32_t slotRow) noexcept {
    if (slot.slotType != format::kDeviceSlotType
        || slot.componentClass != format::kDeviceComponentClass
        || slot.senseSchema != format::kDeviceSenseSchema
        || slot.authSchema != format::kDeviceAuthSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return;
    }
    if (g_deviceSlot != slotRow) {
        g_deviceSlot = slotRow;
        g_deviceChannel = 0;
        g_deviceValue = 1.0F;
        g_deviceSnap = false;
        g_hasDeviceResult = false;
    }
    constexpr std::array<const char*, scriptable_auth::kType23ChannelCount> kChannels{
        "Position", "Power", "Lock"};
    if (g_deviceChannel < 0 || static_cast<std::size_t>(g_deviceChannel) >= kChannels.size()) {
        g_deviceChannel = 0;
    }
    ImGui::SeparatorText("Set channel");
    ImGui::PushID("sdk_device_action");
    ImGui::SetNextItemWidth(230.0F);
    (void)ImGui::Combo(
        "Channel", &g_deviceChannel, kChannels.data(), static_cast<int>(kChannels.size()));
    ImGui::SetNextItemWidth(230.0F);
    (void)ImGui::SliderFloat("Normalized value", &g_deviceValue, 0.0F, 1.0F, "%.2f");
    ImGui::Checkbox("Snap immediately", &g_deviceSnap);
    const auto channel = static_cast<scriptable_auth::Type23Channel>(g_deviceChannel);
    const devices::Status available =
        devices::availability(view, slotRow, channel, g_deviceValue, g_deviceSnap);
    ImGui::BeginDisabled(available != devices::Status::ready);
    if (ImGui::Button("Set channel")) {
        g_deviceResult = devices::set_channel(view, slotRow, channel, g_deviceValue, g_deviceSnap);
        g_hasDeviceResult = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("availability: %s", devices::status_name(available));
    if (g_hasDeviceResult) {
        ImGui::Text("Set result  %s", devices::status_name(g_deviceResult));
    }
    ImGui::TextDisabled("0 to 1. Each end is authored per device.");
    ImGui::PopID();
}

/** Draws the exact generated SDK type-31 action used by Lua `SlotView:fire_trigger`. */
void draw_trigger_action(const sdk::BoundView& view,
                         const format::Slot& slot,
                         std::uint32_t slotRow) noexcept {
    if (slot.slotType != scriptable_auth::kType31SlotType
        || slot.authSchema != scriptable_auth::kType31Schema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return;
    }
    if (g_triggerSlot != slotRow) {
        g_triggerSlot = slotRow;
        g_hasTriggerResult = false;
    }
    ImGui::SeparatorText("Fire trigger");
    ImGui::PushID("sdk_trigger_action");
    const devices::Status available = devices::trigger_availability(view, slotRow);
    ImGui::BeginDisabled(available != devices::Status::ready);
    if (ImGui::Button("Fire trigger")) {
        g_triggerResult = devices::fire_trigger(view, slotRow);
        g_hasTriggerResult = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("availability: %s", devices::status_name(available));
    if (g_hasTriggerResult) {
        ImGui::Text("Fire result  %s", devices::status_name(g_triggerResult));
    }
    ImGui::PopID();
}

/** Draws the selected object's exact reusable slot symbols. */
void draw_slots(const sdk::Catalog& catalog,
                const format::Occurrence& occurrence,
                const format::Object& object,
                const SearchQuery& query) noexcept {
    const auto slots = sdk::object_slots(catalog, object);
    if (g_selectedSlot >= slots.size()) {
        g_selectedSlot = slots.empty() ? format::kAbsentIndex : 0;
    }
    g_filteredRows.clear();
    g_filteredRows.reserve(slots.size());
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (selected_slot_matches(catalog, query, occurrence, object, slots[index])) {
            g_filteredRows.push_back(static_cast<std::uint32_t>(index));
        }
    }
    ImGui::Text("%zu of %zu slots", g_filteredRows.size(), slots.size());
    if (!ImGui::BeginTable(
            "##sdk_slots", 8, kWideTableFlags, table_layout::size(g_filteredRows.size()))) {
        return;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("index");
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("class");
    ImGui::TableSetupColumn("sense schema");
    ImGui::TableSetupColumn("auth schema");
    ImGui::TableSetupColumn("flags");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(clipped_count(g_filteredRows.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::size_t index = g_filteredRows[static_cast<std::size_t>(row)];
            const format::Slot& slot = slots[index];
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(index));
            const std::string_view name = catalog.string(slot.name);
            std::array<char, 160> label{};
            (void)std::snprintf(
                label.data(), label.size(), "%.*s", display_length(name), display_data(name));
            if (table_layout::selectable(label.data(),
                                         g_selectedSlot == static_cast<std::uint32_t>(index))) {
                g_selectedSlot = static_cast<std::uint32_t>(index);
            }
            ImGui::TableNextColumn();
            draw_string(catalog, slot.id);
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(slot.slotIndex));
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(slot.slotType));
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(slot.componentClass));
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(slot.senseSchema));
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(slot.authSchema));
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(slot.flags));
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

/** Draws all evidence gates and their generated refusal reason codes. */
void draw_capability_evidence(const sdk::Catalog& catalog,
                              const format::Capability& capability) noexcept {
    const auto gates = sdk::capability_gates(catalog, capability);
    if (gates.empty()) {
        ImGui::TextDisabled("No gates");
    } else if (ImGui::BeginTable(
                   "##sdk_gates", 6, kWideTableFlags, table_layout::size(gates.size()))) {
        ImGui::TableSetupColumn("gate");
        ImGui::TableSetupColumn("status");
        ImGui::TableSetupColumn("reason code", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("required", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("observed", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("would confirm", ImGuiTableColumnFlags_WidthStretch);
        table_layout::frozen_headers();
        for (const format::Gate& gate : gates) {
            table_layout::next_row();
            ImGui::TableNextColumn();
            draw_string(catalog, gate.gate);
            ImGui::TableNextColumn();
            draw_string(catalog, gate.status);
            ImGui::TableNextColumn();
            draw_string(catalog, gate.reasonCode);
            ImGui::TableNextColumn();
            draw_string(catalog, gate.required);
            ImGui::TableNextColumn();
            draw_string(catalog, gate.observed);
            ImGui::TableNextColumn();
            draw_string(catalog, gate.wouldConfirm);
        }
        ImGui::EndTable();
    }

    const auto refusals = sdk::capability_refusals(catalog, capability);
    if (refusals.empty()) {
        ImGui::TextDisabled("No exposure refusals");
        return;
    }
    ImGui::Text("%zu exposure refusal%s", refusals.size(), refusals.size() == 1 ? "" : "s");
    for (std::size_t index = 0; index < refusals.size(); ++index) {
        const format::Refusal& refusal = refusals[index];
        const std::string_view exposure = catalog.string(refusal.exposure);
        const std::string_view status = catalog.string(refusal.status);
        ImGui::PushID(static_cast<int>(index));
        const bool expanded = ImGui::TreeNodeEx("##sdk_refusal",
                                                ImGuiTreeNodeFlags_SpanAvailWidth,
                                                "%.*s: %.*s",
                                                display_length(exposure),
                                                display_data(exposure),
                                                display_length(status),
                                                display_data(status));
        if (expanded) {
            draw_labeled_string("Refusal ID", catalog, refusal.id);
            const auto reasons = sdk::refusal_reason_codes(catalog, refusal);
            for (const format::Text& reason : reasons) {
                ImGui::Bullet();
                ImGui::SameLine();
                draw_string(catalog, reason.value);
            }
            if (reasons.empty()) {
                ImGui::TextDisabled("No reason codes");
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

/** @return True when any evidence string on one capability gate matches. */
[[nodiscard]] bool gate_matches(const sdk::Catalog& catalog,
                                const SearchQuery& query,
                                const format::Gate& gate) noexcept {
    return query.matches(catalog, gate.gate) || query.matches(catalog, gate.status)
           || query.matches(catalog, gate.reasonCode) || query.matches(catalog, gate.required)
           || query.matches(catalog, gate.observed) || query.matches(catalog, gate.wouldConfirm);
}

/** @return True when one refusal identity, exposure, status, or reason matches. */
[[nodiscard]] bool refusal_matches(const sdk::Catalog& catalog,
                                   const SearchQuery& query,
                                   const format::Refusal& refusal) noexcept {
    return query.matches(catalog, refusal.id) || query.matches(catalog, refusal.exposure)
           || query.matches(catalog, refusal.status)
           || aliases_match(catalog, query, sdk::refusal_reason_codes(catalog, refusal));
}

/** @return True when one capability or any gate/refusal evidence beneath it matches. */
[[nodiscard]] bool capability_matches(const sdk::Catalog& catalog,
                                      const SearchQuery& query,
                                      const format::Capability& capability) noexcept {
    if (query.empty() || query.matches(catalog, capability.id)
        || query.matches(catalog, capability.operation)
        || query.matches(catalog, capability.valueSchemaId)
        || query.matches(subject_kind_name(capability.subjectKind))
        || query.matches("subject kind", capability.subjectKind)
        || query.matches("subject index", capability.subjectIndex)
        || query.matches("exposure flags", capability.exposureFlags)
        || query.matches("candidate exposure flags", capability.candidateExposureFlags)) {
        return true;
    }
    std::array<char, 96> exposure{};
    std::array<char, 96> candidates{};
    format_exposures(capability.exposureFlags, exposure);
    format_exposures(capability.candidateExposureFlags, candidates);
    std::array<char, 128> labeledExposure{};
    std::array<char, 128> labeledCandidates{};
    (void)std::snprintf(
        labeledExposure.data(), labeledExposure.size(), "exposure %s", exposure.data());
    (void)std::snprintf(labeledCandidates.data(),
                        labeledCandidates.size(),
                        "candidate exposure %s",
                        candidates.data());
    if (query.matches(labeledExposure.data()) || query.matches(labeledCandidates.data())) {
        return true;
    }
    for (const format::Gate& gate : sdk::capability_gates(catalog, capability)) {
        if (gate_matches(catalog, query, gate)) {
            return true;
        }
    }
    for (const format::Refusal& refusal : sdk::capability_refusals(catalog, capability)) {
        if (refusal_matches(catalog, query, refusal)) {
            return true;
        }
    }
    return false;
}

/** Draws candidate operation, exposure, gate, and refusal rows. */
void draw_capabilities(const sdk::Catalog& catalog,
                       std::span<const format::Capability> capabilities,
                       const char* emptyText,
                       const SearchQuery& query) noexcept {
    g_filteredRows.clear();
    g_filteredRows.reserve(capabilities.size());
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
        if (capability_matches(catalog, query, capabilities[index])) {
            g_filteredRows.push_back(static_cast<std::uint32_t>(index));
        }
    }
    ImGui::Text("%zu of %zu operations", g_filteredRows.size(), capabilities.size());
    if (capabilities.empty()) {
        ImGui::TextDisabled("%s", emptyText);
        return;
    }
    if (g_filteredRows.empty()) {
        ImGui::TextDisabled("Nothing matches this search");
        return;
    }
    for (const std::uint32_t index : g_filteredRows) {
        const format::Capability& capability = capabilities[index];
        const std::string_view operation = catalog.string(capability.operation);
        std::array<char, 96> exposure{};
        std::array<char, 96> candidates{};
        format_exposures(capability.exposureFlags, exposure);
        format_exposures(capability.candidateExposureFlags, candidates);
        ImGui::PushID(static_cast<int>(index));
        const bool expanded = ImGui::TreeNodeEx("##sdk_capability",
                                                ImGuiTreeNodeFlags_SpanAvailWidth,
                                                "%.*s  candidate: %s",
                                                display_length(operation),
                                                display_data(operation),
                                                candidates.data());
        if (expanded) {
            draw_labeled_string("Capability ID", catalog, capability.id);
            draw_labeled_string("Value schema", catalog, capability.valueSchemaId);
            if (capability.subjectKind
                == static_cast<std::uint32_t>(format::SubjectKind::hostApi)) {
                ImGui::TextUnformatted("Subject host API");
            } else {
                ImGui::Text("Subject %s row %u",
                            subject_kind_name(capability.subjectKind),
                            static_cast<unsigned>(capability.subjectIndex));
            }
            ImGui::Text("Exposure  %s", exposure.data());
            ImGui::Text("Candidate exposure  %s", candidates.data());
            draw_capability_evidence(catalog, capability);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

/** Draws the selected occurrence, object, slot, and operation chain. */
void draw_selected_object(const sdk::BoundView& view,
                          std::span<const format::Occurrence> occurrences,
                          const SearchQuery& query) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    if (g_selectedOccurrence >= occurrences.size()) {
        ImGui::TextDisabled("No occurrence selected");
        return;
    }
    const format::Occurrence& occurrence = occurrences[g_selectedOccurrence];
    const auto objects = catalog.objects();
    if (occurrence.objectIndex >= objects.size()) {
        ImGui::Text("Object row %u is invalid", static_cast<unsigned>(occurrence.objectIndex));
        return;
    }
    const format::Object& object = objects[occurrence.objectIndex];
    ImGui::SeparatorText("Selected object");
    if (!occurrence_matches(catalog, query, occurrence)) {
        ImGui::TextDisabled("This object is outside the search.");
    }
    draw_labeled_string("Occurrence ID", catalog, occurrence.id);
    draw_labeled_string("Object ID", catalog, object.id);
    ImGui::Text("Object row %u  tag 0x%08X  key 0x%08X",
                static_cast<unsigned>(occurrence.objectIndex),
                static_cast<unsigned>(object.objectTag),
                static_cast<unsigned>(object.objectKey));
    ImGui::Text("config %u  descriptor %u  placed subblock %u  leaf %u  hop %u  bare target %u",
                static_cast<unsigned>(object.configCount),
                static_cast<unsigned>(object.descriptorCount),
                static_cast<unsigned>(object.placedSubblockCount),
                static_cast<unsigned>(object.placedLeafCount),
                static_cast<unsigned>(object.placedHopCount),
                static_cast<unsigned>(object.bareTargetCount));
    draw_slots(catalog, occurrence, object, query);
    const auto slots = sdk::object_slots(catalog, object);
    if (g_selectedSlot >= slots.size()) {
        return;
    }
    const format::Slot& slot = slots[g_selectedSlot];
    if (!selected_slot_matches(catalog, query, occurrence, object, slot)) {
        ImGui::TextDisabled("This slot is outside the search.");
        return;
    }
    ImGui::SeparatorText("Selected slot");
    draw_labeled_string("Slot ID", catalog, slot.id);
    draw_labeled_string("Slot name", catalog, slot.name);
    draw_labeled_string("Sense schema ID", catalog, slot.senseSchemaId);
    draw_labeled_string("Auth schema ID", catalog, slot.authSchemaId);
    draw_slot_aliases(catalog, slot);
    ImGui::TextUnformatted("Operations");
    ImGui::PushID("slot_capabilities");
    draw_capabilities(
        catalog, sdk::slot_capabilities(catalog, slot), "No operations", SearchQuery{});
    ImGui::PopID();
    const auto allSlots = catalog.slots();
    if (&slot >= allSlots.data() && &slot < allSlots.data() + allSlots.size()) {
        const std::uint32_t slotRow = static_cast<std::uint32_t>(&slot - allSlots.data());
        draw_device_action(view, slot, slotRow);
        draw_trigger_action(view, slot, slotRow);
    }
}

/** Resolves and revalidates one panel selection against the live ActivityClient. */
[[nodiscard]] sdk::Status bind(const host::InstanceSnapshot& instance,
                               sdk::Snapshot catalog,
                               sdk::BoundView& output) noexcept {
    server::bap::ActivityLinkView link{};
    (void)server::bap::activity_link_view(instance.binding, link);
    const sdk::Selection selection{
        instance.binding, link.matchingLinks, link.activityClientGeneration};
    const sdk::Status result = sdk::resolve(std::move(catalog), selection, output);
    if (result != sdk::Status::ready) {
        return result;
    }
    server::bap::ActivityLinkView current{};
    (void)server::bap::activity_link_view(instance.binding, current);
    return sdk::revalidate(
        output, instance.binding, current.matchingLinks, current.activityClientGeneration);
}

/** One page. Each holds one family of rows and only that family's actions. */
enum class Page : std::uint8_t {
    squads,
    idles,
    combatants,
    devices,
    triggers,
    objects,
    scenes,
    dialogue,
    directives,
    objectives,
    cinematics,
    engagement,
    event,
    occupancy,
    lifetime,
    states,
    missionState,
    positions,
    behaviors,
    script,
};

/** One navigation row: its page, its name, and the one line that page prints. */
struct PageRow final {
    Page page{};
    const char* name{};
    const char* summary{};
    marker::WorldPage world{marker::WorldPage::none};
};

/** The navigation order. Actions first, then the read-only pages. */
constexpr std::array<PageRow, 20> kPages{{
    {Page::squads, "Squads", "Place an authored squad.", marker::WorldPage::squads},
    {Page::idles, "Idles", "Start an actor's authored state.", marker::WorldPage::squads},
    {Page::combatants, "Combatants", "Bind a combatant, retain a channel."},
    {Page::devices, "Devices", "Drive a door, lift or switch.", marker::WorldPage::devices},
    {Page::triggers, "Triggers", "Fire an authored pulse.", marker::WorldPage::triggers},
    {Page::objects, "Objects", "Spawn or remove a placed object.", marker::WorldPage::objects},
    {Page::scenes, "Scenes", "Activate an authored scene."},
    {Page::dialogue, "Dialogue", "Play an authored cue."},
    {Page::directives, "Directives", "Show or clear a HUD directive."},
    {Page::objectives, "Objectives", "Reset objectives, advance a task."},
    {Page::cinematics, "Cinematics", "Play a sequence or cinematic."},
    {Page::engagement, "Engagement", "Set the encounter engagement body."},
    {Page::event, "Public event", "Watch one player against an event area."},
    {Page::occupancy, "Occupancy", "Count an object filter into Sense."},
    {Page::lifetime, "Lifetime", "Set the activity lifetime state."},
    {Page::states, "States", "Select one authored state by region."},
    {Page::missionState, "Mission state", "Durable variables and timers."},
    {Page::positions,
     "Positions",
     "Package positions no slot claims.",
     marker::WorldPage::positions},
    {Page::behaviors, "Behaviors", "Compiled behavior roots. No action."},
    {Page::script, "Script", "Mission VM state and its output lane."},
}};

/** Page the navigation is on. It survives a rebind so an operator keeps their place. */
Page g_page{Page::squads};

/** Draws the navigation column and returns the page it leaves selected. */
[[nodiscard]] const PageRow& draw_navigation() noexcept {
    const float width = scaling::pixels(kNavigationWidth);
    if (ImGui::BeginChild("##activity_sdk_nav", {width, 0.0F}, ImGuiChildFlags_Borders)) {
        for (const PageRow& row : kPages) {
            if (ImGui::Selectable(row.name, row.page == g_page)) {
                g_page = row.page;
            }
        }
    }
    ImGui::EndChild();
    for (const PageRow& row : kPages) {
        if (row.page == g_page) {
            return row;
        }
    }
    return kPages.front();
}

/** Draws the one selected page inside the frame the navigation left. */
void draw_page(const PageRow& row,
               const sdk::BoundView& view,
               const format::Scenario& scenario,
               const host::InstanceSnapshot& instance) noexcept {
    switch (row.page) {
    case Page::squads:
        sdk_squad_view::draw(view, scenario);
        return;
    case Page::idles:
        sdk_state_pages::draw_performances(view);
        return;
    case Page::combatants:
        sdk_state_pages::draw_combatants(view);
        return;
    case Page::devices:
        scriptable_browser::draw_devices(&instance);
        return;
    case Page::triggers:
        scriptable_browser::draw_triggers(&instance);
        return;
    case Page::objects:
        scriptable_browser::draw_objects(&instance);
        return;
    case Page::scenes:
        sdk_mission_view::draw_scenes(view);
        return;
    case Page::dialogue:
        sdk_mission_view::draw_dialogue(view);
        return;
    case Page::directives:
        sdk_mission_view::draw_directives(view);
        return;
    case Page::objectives:
        sdk_mission_view::draw_objectives(view);
        return;
    case Page::cinematics:
        sdk_mission_view::draw_cinematics(view);
        return;
    case Page::engagement:
        sdk_state_pages::draw_engagement(view);
        return;
    case Page::event:
        sdk_state_pages::draw_public_events(view);
        return;
    case Page::occupancy:
        sdk_state_pages::draw_occupancy(view);
        return;
    case Page::lifetime:
        sdk_state_pages::draw_lifetime(view);
        return;
    case Page::states:
        sdk_state_pages::draw_states(view);
        return;
    case Page::missionState:
        sdk_state_pages::draw_mission_state(view);
        return;
    case Page::positions:
        scriptable_browser::draw_positions(&instance);
        return;
    case Page::behaviors:
        sdk_mission_view::draw_compiled_behaviors(view);
        return;
    case Page::script:
        draw_script_runtime(instance);
        return;
    }
}

/** Draws only data from one resolved immutable generated SDK view. */
void draw_bound(const sdk::BoundView& view,
                const format::Scenario& scenario,
                const host::InstanceSnapshot& instance) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    const auto occurrences = sdk::scenario_occurrences(catalog, scenario);
    sync_selection(view, occurrences);
    const PageRow& row = draw_navigation();
    marker::set_world_page(row.world);
    ImGui::SameLine();
    if (ImGui::BeginChild("##activity_sdk_page", {0.0F, 0.0F})) {
        ImGui::TextDisabled("%s", row.summary);
        ImGui::PushID(row.name);
        draw_page(row, view, scenario, instance);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

/** Draws one compact line for the generator and the installed pack. */
void draw_status_line(sdk::Status loadStatus) noexcept {
    const generation::Snapshot generationState = generation::snapshot();
    ImGui::Text("SDK %s", sdk::status_name(loadStatus));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", generation::status_name(generationState.status));
    if (generationState.total != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%u/%u",
                            static_cast<unsigned>(generationState.current),
                            static_cast<unsigned>(generationState.total));
    }
    if (generationState.detail[0] != '\0') {
        if (generationState.status == generation::Status::failed) {
            ImGui::Text("Generation failed: %s", generationState.detail.data());
        } else {
            ImGui::TextDisabled("%s", generationState.detail.data());
        }
    }
}

/** Draws loader and exact-binding refusal status, then the resolved SDK. */
void draw_content(const host::InstanceSnapshot* instance) noexcept {
    scriptable_browser::prepare(instance);
    const sdk::Status loadStatus = sdk::status();
    draw_status_line(loadStatus);
    const sdk::Snapshot catalog = sdk::snapshot();
    if (loadStatus != sdk::Status::ready) {
        ImGui::TextDisabled("No activity SDK is published.");
        ImGui::SeparatorText("World objects");
        marker::set_world_page(marker::WorldPage::objects);
        scriptable_browser::draw_objects(instance);
        return;
    }
    if (!catalog) {
        ImGui::Text("SDK resolve  %s", sdk::status_name(sdk::Status::catalogInvalid));
        ImGui::SeparatorText("World objects");
        marker::set_world_page(marker::WorldPage::objects);
        scriptable_browser::draw_objects(instance);
        return;
    }
    if (instance == nullptr) {
        ImGui::TextDisabled("No activity selected.");
        marker::set_world_page(marker::WorldPage::none);
        return;
    }
    sdk::BoundView view{};
    const sdk::Status resolveStatus = bind(*instance, catalog, view);
    if (resolveStatus != sdk::Status::ready) {
        ImGui::Text("Bind refused  %s", sdk::status_name(resolveStatus));
        marker::set_world_page(marker::WorldPage::none);
        return;
    }
    const format::Activity* const activity = sdk::bound_activity(view);
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    if (activity == nullptr || scenario == nullptr) {
        ImGui::Text("Bind refused  %s", sdk::status_name(sdk::Status::catalogInvalid));
        marker::set_world_page(marker::WorldPage::none);
        return;
    }
    draw_bound(view, *scenario, *instance);
}

} // namespace

/** Draws World content inside the Core-owned active tab. */
void draw(const host::InstanceSnapshot* instance) noexcept {
    draw_content(instance);
}
void deactivate() noexcept {
    marker::deactivate_world_page();
}

} // namespace sunrise::server::ui::activity_host::sdk_view
