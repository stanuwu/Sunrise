#include "izanami_panel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../catalog/catalog_record.h"
#include "../../editor/workspace/editor_workspace.h"
#include "../../fate/lexer/lexer.h"
#include "../../fate/parser/parser.h"
#include "../../kernel/kernel.h"
#include "../../project/serialization/schema.h"
#include "../../research/experiments.h"
#include "../../research/native_targets.h"
#include "../../runtime/runtime_adapter.h"

namespace sunrise::izanami::editor::ui {
namespace {

namespace catalog = sunrise::izanami::catalog;
namespace lexer = sunrise::izanami::fate::lexer;
namespace parser = sunrise::izanami::fate::parser;
namespace kernel = sunrise::izanami::kernel;
namespace serialization = sunrise::izanami::project::serialization;
namespace research = sunrise::izanami::research;
namespace runtime = sunrise::izanami::runtime;
namespace workspace = sunrise::izanami::editor::workspace;

constexpr std::string_view kSampleFate = "entity DoorController { on start { } }";
constexpr char kObjectDragPayload[] = "IZANAMI_OBJECT";
constexpr UINT kStandaloneToggleKey = VK_F8;
constexpr float kStandaloneViewportScale = 0.86F;
constexpr ImGuiWindowFlags kStandaloneWindowFlags =
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

std::atomic_bool g_standaloneVisible{};

struct CapabilityRow {
    runtime::Capability capability;
    std::string_view label;
};

constexpr std::array kCapabilityRows{
    CapabilityRow{runtime::Capability::worldStaticSpawn, "world.static.spawn"},
    CapabilityRow{runtime::Capability::worldPatternSpawn, "world.pattern.spawn"},
    CapabilityRow{runtime::Capability::worldEntitySpawnLoaded, "world.entity.spawn.loaded"},
    CapabilityRow{runtime::Capability::worldDestroy, "world.destroy"},
    CapabilityRow{runtime::Capability::worldTransformWrite, "world.transform.write"},
    CapabilityRow{runtime::Capability::physicsRaycastPosition, "physics.raycast.position"},
};

struct ObjectKindOption {
    core::ObjectKind kind;
    std::string_view label;
    std::uint32_t defaultClassId;
};

constexpr std::array kObjectKindOptions{
    ObjectKindOption{core::ObjectKind::forgeOnly, "Forge-only", 0},
    ObjectKindOption{core::ObjectKind::staticInstance, "Static", catalog::kStaticMeshClassId},
    ObjectKindOption{core::ObjectKind::patternInstance, "Pattern", catalog::kPatternClassId},
    ObjectKindOption{core::ObjectKind::entityInstance, "Entity", catalog::kEntityClassId},
    ObjectKindOption{core::ObjectKind::folder, "Folder", 0},
};

struct EditorUiState {
    char hierarchyFilter[96]{};
    char createName[96]{"Forge Object"};
    char renameName[128]{};
    core::ForgeUUID renameObject{};
    int createKindIndex{};
    std::uint32_t createClassId{catalog::kStaticMeshClassId};
    std::uint32_t createTagHash{};
    std::uint64_t createWideHash{};
    bool parentNewObjectsToSelection{};
    bool liveEditsOnly{true};
    bool snapEnabled{true};
    float snapStep{1.0F};
    float viewportZoom{26.0F};
    ImVec2 viewportPan{};
    bool draggingObject{};
    core::ForgeUUID dragObject{};
    core::Transform dragStartTransform{};
    ImVec2 dragStartWorld{};
};

void text(std::string_view value) noexcept {
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

[[nodiscard]] EditorUiState& ui_state() noexcept {
    static EditorUiState state;
    return state;
}

void report_overlay_visibility(bool visible) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=izanami_overlay stage=visibility result=%s vk=0x%X",
                                      visible ? "open" : "closed",
                                      static_cast<unsigned>(kStandaloneToggleKey));
    if (written > 0) {
        ::sunrise::core::log::write(::sunrise::core::log::Channel::client,
                                    ::sunrise::core::log::Level::info,
                                    {line.data(), static_cast<std::size_t>(written)});
    }
}

template <std::size_t Size>
void copy_to_buffer(char (&buffer)[Size], std::string_view value) noexcept {
    static_assert(Size > 0);
    const std::size_t count = (std::min)(Size - 1, value.size());
    if (count > 0) {
        std::memcpy(buffer, value.data(), count);
    }
    buffer[count] = '\0';
}

[[nodiscard]] std::string_view evidence_text(research::EvidenceStatus status) noexcept {
    switch (status) {
    case research::EvidenceStatus::unknown:
        return "unknown";
    case research::EvidenceStatus::historicalLead:
        return "historical lead";
    case research::EvidenceStatus::openPrLead:
        return "open PR lead";
    case research::EvidenceStatus::mergedVerified:
        return "merged verified";
    case research::EvidenceStatus::currentBuildVerified:
        return "current build verified";
    }
    return "unknown";
}

[[nodiscard]] std::string_view kind_text(core::ObjectKind kind) noexcept {
    switch (kind) {
    case core::ObjectKind::forgeOnly:
        return "Forge-only";
    case core::ObjectKind::staticInstance:
        return "Static";
    case core::ObjectKind::patternInstance:
        return "Pattern";
    case core::ObjectKind::entityInstance:
        return "Entity";
    case core::ObjectKind::folder:
        return "Folder";
    }
    return "Unknown";
}

[[nodiscard]] std::string_view default_name_for_kind(core::ObjectKind kind) noexcept {
    switch (kind) {
    case core::ObjectKind::forgeOnly:
        return "Forge Object";
    case core::ObjectKind::staticInstance:
        return "Static Instance";
    case core::ObjectKind::patternInstance:
        return "Pattern Instance";
    case core::ObjectKind::entityInstance:
        return "Entity Instance";
    case core::ObjectKind::folder:
        return "Folder";
    }
    return "Forge Object";
}

[[nodiscard]] int kind_index(core::ObjectKind kind) noexcept {
    for (std::size_t index = 0; index < kObjectKindOptions.size(); ++index) {
        if (kObjectKindOptions[index].kind == kind) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

[[nodiscard]] core::ObjectKind kind_from_index(int index) noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= kObjectKindOptions.size()) {
        return core::ObjectKind::forgeOnly;
    }
    return kObjectKindOptions[static_cast<std::size_t>(index)].kind;
}

[[nodiscard]] std::uint32_t default_class_for_kind(core::ObjectKind kind) noexcept {
    for (const ObjectKindOption& option : kObjectKindOptions) {
        if (option.kind == kind) {
            return option.defaultClassId;
        }
    }
    return 0;
}

[[nodiscard]] std::array<char, 17> uuid_suffix(core::ForgeUUID id) noexcept {
    std::array<char, 17> buffer{};
    (void)std::snprintf(buffer.data(),
                        buffer.size(),
                        "%02X%02X%02X%02X%02X%02X%02X%02X",
                        static_cast<unsigned int>(id.bytes[8]),
                        static_cast<unsigned int>(id.bytes[9]),
                        static_cast<unsigned int>(id.bytes[10]),
                        static_cast<unsigned int>(id.bytes[11]),
                        static_cast<unsigned int>(id.bytes[12]),
                        static_cast<unsigned int>(id.bytes[13]),
                        static_cast<unsigned int>(id.bytes[14]),
                        static_cast<unsigned int>(id.bytes[15]));
    return buffer;
}

[[nodiscard]] std::array<char, 112> resource_text(core::ResourceId resource) noexcept {
    std::array<char, 112> buffer{};
    if (!resource.is_valid()) {
        (void)std::snprintf(buffer.data(), buffer.size(), "none");
        return buffer;
    }
    (void)std::snprintf(buffer.data(),
                        buffer.size(),
                        "class %08X / tag %08X / wide %016llX",
                        static_cast<unsigned int>(resource.classId),
                        static_cast<unsigned int>(resource.tagHash),
                        static_cast<unsigned long long>(resource.wideHash));
    return buffer;
}

[[nodiscard]] std::string_view
runtime_label(const project::scene::ForgeObject& object,
              const workspace::ObjectRuntimeBinding* binding) noexcept {
    if (binding == nullptr) {
        return object.kind == core::ObjectKind::forgeOnly || object.kind == core::ObjectKind::folder
                   ? "local"
                   : "unbound";
    }
    if (binding->handle.is_valid()) {
        return "live";
    }
    if (object.kind == core::ObjectKind::forgeOnly || object.kind == core::ObjectKind::folder) {
        return "local";
    }
    return runtime::status_text(binding->lastStatus);
}

[[nodiscard]] bool runtime_can_spawn_kind(workspace::EditorWorkspace& editor,
                                          core::ObjectKind kind) noexcept {
    const runtime::CapabilitySet capabilities = editor.runtime_capabilities();
    switch (kind) {
    case core::ObjectKind::staticInstance:
        return capabilities.has(runtime::Capability::worldStaticSpawn);
    case core::ObjectKind::patternInstance:
        return capabilities.has(runtime::Capability::worldPatternSpawn);
    case core::ObjectKind::entityInstance:
        return capabilities.has(runtime::Capability::worldEntitySpawnLoaded);
    case core::ObjectKind::forgeOnly:
    case core::ObjectKind::folder:
        return false;
    }
    return false;
}

[[nodiscard]] bool can_place_kind(workspace::EditorWorkspace& editor,
                                  const EditorUiState& state,
                                  core::ObjectKind kind) noexcept {
    return !state.liveEditsOnly || runtime_can_spawn_kind(editor, kind);
}

[[nodiscard]] bool can_write_live_transform(workspace::EditorWorkspace& editor,
                                            const project::scene::ForgeObject& object) noexcept {
    const workspace::ObjectRuntimeBinding* const binding = editor.runtime_binding(object.id);
    return binding != nullptr && binding->handle.is_valid()
           && editor.runtime_capabilities().has(runtime::Capability::worldTransformWrite);
}

[[nodiscard]] bool can_edit_transform(workspace::EditorWorkspace& editor,
                                      const EditorUiState& state,
                                      const project::scene::ForgeObject& object) noexcept {
    return !state.liveEditsOnly || can_write_live_transform(editor, object);
}

[[nodiscard]] std::string_view edit_mode_text(const EditorUiState& state) noexcept {
    return state.liveEditsOnly ? "live runtime only" : "local preview";
}

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

[[nodiscard]] bool contains_lower(std::string_view haystack, std::string_view needleLower) {
    if (needleLower.empty()) {
        return true;
    }
    return lowercase(haystack).find(needleLower) != std::string::npos;
}

[[nodiscard]] const project::scene::ForgeObject*
find_object(std::span<const project::scene::ForgeObject> objects, core::ForgeUUID id) noexcept {
    for (const project::scene::ForgeObject& object : objects) {
        if (object.id == id) {
            return &object;
        }
    }
    return nullptr;
}

[[nodiscard]] bool direct_match(const project::scene::ForgeObject& object,
                                std::string_view filterLower) {
    const std::string_view name = object.editorName.empty() ? "Unnamed" : object.editorName;
    const std::array<char, 112> resource = resource_text(object.resource);
    const std::array<char, 17> id = uuid_suffix(object.id);
    return contains_lower(name, filterLower) || contains_lower(kind_text(object.kind), filterLower)
           || contains_lower(resource.data(), filterLower)
           || contains_lower(id.data(), filterLower);
}

[[nodiscard]] bool descendant_match(std::span<const project::scene::ForgeObject> objects,
                                    core::ForgeUUID parent,
                                    std::string_view filterLower) {
    for (const project::scene::ForgeObject& object : objects) {
        if (object.parent == parent
            && (direct_match(object, filterLower)
                || descendant_match(objects, object.id, filterLower))) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool visible_for_filter(std::span<const project::scene::ForgeObject> objects,
                                      const project::scene::ForgeObject& object,
                                      std::string_view filterLower) {
    return filterLower.empty() || direct_match(object, filterLower)
           || descendant_match(objects, object.id, filterLower);
}

[[nodiscard]] bool has_visible_children(std::span<const project::scene::ForgeObject> objects,
                                        const project::scene::ForgeObject& parent,
                                        std::string_view filterLower) {
    for (const project::scene::ForgeObject& object : objects) {
        if (object.parent == parent.id && visible_for_filter(objects, object, filterLower)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::array<char, 224>
object_label(const project::scene::ForgeObject& object,
             const workspace::ObjectRuntimeBinding* binding) noexcept {
    std::array<char, 224> buffer{};
    const std::string_view name = object.editorName.empty() ? "Unnamed" : object.editorName;
    const std::array<char, 17> id = uuid_suffix(object.id);
    const std::string_view runtime = runtime_label(object, binding);
    (void)std::snprintf(buffer.data(),
                        buffer.size(),
                        "%s%s%.*s  [%.*s/%.*s]##%s",
                        object.editorVisible ? "" : "[hidden] ",
                        object.editorLocked ? "[locked] " : "",
                        static_cast<int>(name.size()),
                        name.data(),
                        static_cast<int>(kind_text(object.kind).size()),
                        kind_text(object.kind).data(),
                        static_cast<int>(runtime.size()),
                        runtime.data(),
                        id.data());
    return buffer;
}

[[nodiscard]] bool draw_kind_combo(const char* label, core::ObjectKind& kind) {
    bool changed = false;
    if (ImGui::BeginCombo(label, kind_text(kind).data())) {
        for (const ObjectKindOption& option : kObjectKindOptions) {
            const bool selected = option.kind == kind;
            if (ImGui::Selectable(option.label.data(), selected)) {
                kind = option.kind;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

[[nodiscard]] core::Transform default_transform() noexcept {
    core::Transform transform{};
    transform.uniformScale = 1.0F;
    return transform;
}

[[nodiscard]] core::ResourceId create_resource_from_state(const EditorUiState& state,
                                                          core::ObjectKind kind) noexcept {
    if (kind == core::ObjectKind::forgeOnly || kind == core::ObjectKind::folder) {
        return {};
    }
    core::ResourceId resource;
    resource.classId =
        state.createClassId == 0 ? default_class_for_kind(kind) : state.createClassId;
    resource.tagHash = state.createTagHash;
    resource.wideHash = state.createWideHash;
    return resource;
}

void create_object_from_toolbar(workspace::EditorWorkspace& editor,
                                core::ObjectKind kind,
                                std::string_view name) {
    core::ResourceId resource{};
    if (kind != core::ObjectKind::forgeOnly && kind != core::ObjectKind::folder) {
        resource.classId = default_class_for_kind(kind);
    }
    (void)editor.create_object(std::string{name}, kind, resource, default_transform(), {});
}

void draw_launcher(workspace::EditorWorkspace& editor) noexcept {
    ImGui::TextUnformatted("Open Forge Editor");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Choose a baseplate or bubble template. The editor opens as a Forge-authored scene first; "
        "Destiny runtime hooks are explicit experiments.");
    ImGui::Spacing();

    const std::span<const workspace::BaseplateTemplate> templates = editor.templates();
    const std::size_t selected = editor.selected_template_index();
    if (ImGui::BeginTable("izanami_template_picker", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Template", ImGuiTableColumnFlags_WidthStretch, 0.4F);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.6F);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        for (std::size_t index = 0; index < templates.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(templates[index].displayName.data(), index == selected)) {
                (void)editor.select_template(index);
            }
            ImGui::PopID();
        }
        ImGui::TableSetColumnIndex(1);
        const workspace::BaseplateTemplate& current =
            templates[selected < templates.size() ? selected : 0];
        text(current.displayName);
        ImGui::Text("Destination: %s", current.destinationHint.data());
        ImGui::Text("Bubble: %s", current.bubbleHint.data());
        ImGui::Text("Redirect target: %s", current.hasLaunchTarget ? "available" : "not validated");
        ImGui::TextWrapped("%s", current.description.data());
        ImGui::EndTable();
    }

    ImGui::Spacing();
    const workspace::BaseplateTemplate& selectedTemplate =
        templates[selected < templates.size() ? selected : 0];
    const bool packageAuthoring = selectedTemplate.id == std::string_view{"blank_baseplate"};
    ImGui::BeginDisabled(!selectedTemplate.hasLaunchTarget && !packageAuthoring);
    if (ImGui::Button(packageAuthoring ? "Build Map Package" : "Launch In Destiny",
                      ImVec2(180.0F, 0.0F))) {
        (void)editor.launch_selected_template();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open Editor Only", ImVec2(180.0F, 0.0F))) {
        (void)editor.open_selected_template();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!templates[selected < templates.size() ? selected : 0].hasLaunchTarget);
    if (ImGui::Button("Arm Redirect")) {
        (void)editor.arm_selected_template_redirect();
    }
    ImGui::SameLine();
    if (ImGui::Button("Probe Direct Launch")) {
        (void)editor.probe_selected_template_launch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Arm + Open Director")) {
        (void)editor.arm_selected_template_redirect();
        (void)editor.request_native_director_handoff();
    }
    ImGui::EndDisabled();
    if (!editor.last_launch_message().empty()) {
        ImGui::Spacing();
        text(editor.last_launch_message());
    }
}

void draw_drag_drop_for_object(workspace::EditorWorkspace& editor,
                               const project::scene::ForgeObject& object) {
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const core::ForgeUUID payload = object.id;
        ImGui::SetDragDropPayload(kObjectDragPayload, &payload, sizeof(payload));
        text(object.editorName.empty() ? "Unnamed" : std::string_view{object.editorName});
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload(kObjectDragPayload);
            payload != nullptr && payload->DataSize == sizeof(core::ForgeUUID)) {
            core::ForgeUUID dropped{};
            std::memcpy(&dropped, payload->Data, sizeof(dropped));
            if (!(dropped == object.id)) {
                (void)editor.reparent_object(dropped, object.id);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void draw_object_node(workspace::EditorWorkspace& editor,
                      std::span<const project::scene::ForgeObject> objects,
                      const project::scene::ForgeObject& object,
                      std::string_view filterLower) {
    if (!visible_for_filter(objects, object, filterLower)) {
        return;
    }

    const bool hasChildren = has_visible_children(objects, object, filterLower);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    if (object.id == editor.selected_id()) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (!hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const std::array<char, 224> label = object_label(object, editor.runtime_binding(object.id));
    const bool open = ImGui::TreeNodeEx(label.data(), flags);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        (void)editor.select(object.id);
    }
    draw_drag_drop_for_object(editor, object);

    if (hasChildren && open) {
        for (const project::scene::ForgeObject& child : objects) {
            if (child.parent == object.id) {
                draw_object_node(editor, objects, child, filterLower);
            }
        }
        ImGui::TreePop();
    }
}

void draw_hierarchy(workspace::EditorWorkspace& editor, EditorUiState& state) {
    const std::span<const project::scene::ForgeObject> objects = editor.objects();
    ImGui::TextUnformatted("Hierarchy");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##izanami_hierarchy_filter",
                             "Search names, kinds, IDs, resources",
                             state.hierarchyFilter,
                             sizeof(state.hierarchyFilter));

    if (ImGui::Selectable("Scene Root", false, ImGuiSelectableFlags_SpanAllColumns)) {}
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload(kObjectDragPayload);
            payload != nullptr && payload->DataSize == sizeof(core::ForgeUUID)) {
            core::ForgeUUID dropped{};
            std::memcpy(&dropped, payload->Data, sizeof(dropped));
            (void)editor.reparent_object(dropped, {});
        }
        ImGui::EndDragDropTarget();
    }

    const std::string filterLower = lowercase(state.hierarchyFilter);
    ImGui::BeginChild("izanami_hierarchy_tree", ImVec2(0.0F, 0.0F), false);
    bool anyVisible = false;
    for (const project::scene::ForgeObject& object : objects) {
        if (object.parent.is_nil() && visible_for_filter(objects, object, filterLower)) {
            anyVisible = true;
            draw_object_node(editor, objects, object, filterLower);
        }
    }
    if (!anyVisible) {
        ImGui::TextUnformatted("No matching objects");
    }
    ImGui::EndChild();
}

[[nodiscard]] float snap_value(float value, const EditorUiState& state) noexcept {
    if (!state.snapEnabled || state.snapStep <= 0.0F) {
        return value;
    }
    return std::round(value / state.snapStep) * state.snapStep;
}

[[nodiscard]] ImVec2
world_to_screen(ImVec2 world, ImVec2 center, const EditorUiState& state) noexcept {
    return {center.x + state.viewportPan.x + world.x * state.viewportZoom,
            center.y + state.viewportPan.y - world.y * state.viewportZoom};
}

[[nodiscard]] ImVec2
screen_to_world(ImVec2 screen, ImVec2 center, const EditorUiState& state) noexcept {
    return {(screen.x - center.x - state.viewportPan.x) / state.viewportZoom,
            -(screen.y - center.y - state.viewportPan.y) / state.viewportZoom};
}

void draw_viewport_grid(ImDrawList* draw,
                        ImVec2 canvasMin,
                        ImVec2 canvasMax,
                        ImVec2 center,
                        const EditorUiState& state) {
    draw->AddRectFilled(canvasMin, canvasMax, IM_COL32(18, 22, 26, 255));
    const float baseWorldStep = state.snapEnabled && state.snapStep > 0.0F ? state.snapStep : 1.0F;
    float pixelStep = baseWorldStep * state.viewportZoom;
    while (pixelStep < 14.0F) {
        pixelStep *= 2.0F;
    }
    const float originX = center.x + state.viewportPan.x;
    const float originY = center.y + state.viewportPan.y;
    const float startX = canvasMin.x + std::fmod(originX - canvasMin.x, pixelStep) - pixelStep;
    const float startY = canvasMin.y + std::fmod(originY - canvasMin.y, pixelStep) - pixelStep;
    for (float x = startX; x < canvasMax.x; x += pixelStep) {
        draw->AddLine({x, canvasMin.y}, {x, canvasMax.y}, IM_COL32(44, 50, 57, 180));
    }
    for (float y = startY; y < canvasMax.y; y += pixelStep) {
        draw->AddLine({canvasMin.x, y}, {canvasMax.x, y}, IM_COL32(44, 50, 57, 180));
    }
    draw->AddLine(
        {canvasMin.x, originY}, {canvasMax.x, originY}, IM_COL32(94, 119, 156, 220), 1.5F);
    draw->AddLine({originX, canvasMin.y}, {originX, canvasMax.y}, IM_COL32(156, 94, 94, 220), 1.5F);
    draw->AddRect(canvasMin, canvasMax, IM_COL32(70, 78, 88, 255));
}

[[nodiscard]] ImU32 color_for_kind(core::ObjectKind kind, bool selected) noexcept {
    if (selected) {
        return IM_COL32(255, 214, 112, 255);
    }
    switch (kind) {
    case core::ObjectKind::forgeOnly:
        return IM_COL32(120, 178, 255, 255);
    case core::ObjectKind::staticInstance:
        return IM_COL32(104, 210, 154, 255);
    case core::ObjectKind::patternInstance:
        return IM_COL32(210, 153, 255, 255);
    case core::ObjectKind::entityInstance:
        return IM_COL32(255, 138, 116, 255);
    case core::ObjectKind::folder:
        return IM_COL32(180, 188, 198, 255);
    }
    return IM_COL32(120, 178, 255, 255);
}

[[nodiscard]] core::ForgeUUID
hit_test_viewport(std::span<const project::scene::ForgeObject> objects,
                  ImVec2 mouse,
                  ImVec2 center,
                  const EditorUiState& state) noexcept {
    core::ForgeUUID hit{};
    float bestDistanceSquared = 14.0F * 14.0F;
    for (const project::scene::ForgeObject& object : objects) {
        if (!object.editorVisible) {
            continue;
        }
        const ImVec2 world{object.transform.translation.x, object.transform.translation.z};
        const ImVec2 screen = world_to_screen(world, center, state);
        const float dx = mouse.x - screen.x;
        const float dy = mouse.y - screen.y;
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= bestDistanceSquared) {
            bestDistanceSquared = distanceSquared;
            hit = object.id;
        }
    }
    return hit;
}

void draw_viewport_objects(workspace::EditorWorkspace& editor,
                           ImDrawList* draw,
                           ImVec2 center,
                           const EditorUiState& state) {
    const std::span<const project::scene::ForgeObject> objects = editor.objects();
    for (const project::scene::ForgeObject& object : objects) {
        if (object.parent.is_nil() || !object.editorVisible) {
            continue;
        }
        const project::scene::ForgeObject* const parent = find_object(objects, object.parent);
        if (parent == nullptr || !parent->editorVisible) {
            continue;
        }
        const ImVec2 childWorld{object.transform.translation.x, object.transform.translation.z};
        const ImVec2 parentWorld{parent->transform.translation.x, parent->transform.translation.z};
        draw->AddLine(world_to_screen(parentWorld, center, state),
                      world_to_screen(childWorld, center, state),
                      IM_COL32(90, 96, 105, 170),
                      1.0F);
    }

    for (const project::scene::ForgeObject& object : objects) {
        if (!object.editorVisible) {
            continue;
        }
        const bool selected = object.id == editor.selected_id();
        const ImVec2 world{object.transform.translation.x, object.transform.translation.z};
        const ImVec2 screen = world_to_screen(world, center, state);
        const bool liveEditable = can_edit_transform(editor, state, object);
        const ImU32 color = state.liveEditsOnly && !liveEditable
                                ? IM_COL32(88, 96, 108, 210)
                                : color_for_kind(object.kind, selected);
        const float radius = object.kind == core::ObjectKind::folder ? 7.0F : 8.5F;
        if (object.kind == core::ObjectKind::folder) {
            draw->AddRectFilled({screen.x - radius, screen.y - radius},
                                {screen.x + radius, screen.y + radius},
                                color,
                                2.0F);
        } else {
            draw->AddCircleFilled(screen, radius, color, 16);
        }
        if (object.editorLocked) {
            draw->AddCircle(screen, radius + 4.0F, IM_COL32(230, 230, 230, 210), 16, 1.0F);
        }
        if (state.liveEditsOnly && !liveEditable) {
            draw->AddCircle(screen, radius + 3.0F, IM_COL32(150, 160, 174, 190), 16, 1.0F);
        }
        if (selected) {
            draw->AddCircle(screen, radius + 6.0F, IM_COL32(255, 255, 255, 255), 20, 2.0F);
        }
        const std::string_view name = object.editorName.empty() ? "Unnamed" : object.editorName;
        draw->AddText({screen.x + 11.0F, screen.y - 7.0F},
                      state.liveEditsOnly && !liveEditable ? IM_COL32(170, 178, 190, 220)
                                                           : IM_COL32(226, 232, 239, 240),
                      name.data(),
                      name.data() + name.size());
    }
}

void handle_viewport_input(workspace::EditorWorkspace& editor,
                           EditorUiState& state,
                           ImVec2 canvasMin,
                           ImVec2 canvasMax,
                           ImVec2 center,
                           bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0.0F) {
        const float oldZoom = state.viewportZoom;
        state.viewportZoom =
            (std::clamp)(state.viewportZoom * (1.0F + io.MouseWheel * 0.12F), 8.0F, 96.0F);
        if (oldZoom > 0.0F) {
            const ImVec2 before = screen_to_world(io.MousePos, center, state);
            state.viewportPan.x += (before.x * oldZoom - before.x * state.viewportZoom);
            state.viewportPan.y -= (before.y * oldZoom - before.y * state.viewportZoom);
        }
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        state.viewportPan.x += io.MouseDelta.x;
        state.viewportPan.y += io.MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const core::ForgeUUID hit = hit_test_viewport(editor.objects(), io.MousePos, center, state);
        if (!hit.is_nil() && editor.select(hit)) {
            const project::scene::ForgeObject* const object = editor.selected_object();
            if (object != nullptr && !object->editorLocked
                && can_edit_transform(editor, state, *object)) {
                state.draggingObject = true;
                state.dragObject = hit;
                state.dragStartTransform = object->transform;
                state.dragStartWorld = screen_to_world(io.MousePos, center, state);
            }
        }
    }
    if (state.draggingObject && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const project::scene::ForgeObject* const object = editor.find_object(state.dragObject);
        if (object != nullptr && !object->editorLocked
            && can_edit_transform(editor, state, *object)) {
            const ImVec2 world = screen_to_world(io.MousePos, center, state);
            core::Transform transform = state.dragStartTransform;
            transform.translation.x = snap_value(
                state.dragStartTransform.translation.x + (world.x - state.dragStartWorld.x), state);
            transform.translation.z = snap_value(
                state.dragStartTransform.translation.z + (world.y - state.dragStartWorld.y), state);
            (void)editor.preview_selected_transform(transform);
        } else {
            state.draggingObject = false;
            state.dragObject = {};
        }
    }
    if (state.draggingObject && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const project::scene::ForgeObject* const object = editor.find_object(state.dragObject);
        if (object != nullptr) {
            (void)editor.commit_selected_transform(state.dragStartTransform, object->transform);
        }
        state.draggingObject = false;
        state.dragObject = {};
    }
    (void)canvasMin;
    (void)canvasMax;
}

void draw_viewport(workspace::EditorWorkspace& editor, EditorUiState& state) {
    ImGui::TextUnformatted("Viewport");
    ImGui::SameLine();
    ImGui::Checkbox("Live edits only", &state.liveEditsOnly);
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &state.snapEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(82.0F);
    ImGui::DragFloat("Step", &state.snapStep, 0.05F, 0.05F, 64.0F, "%.2f");

    const ImVec2 requestedSize{0.0F, 420.0F};
    ImGui::BeginChild("izanami_top_down_viewport",
                      requestedSize,
                      true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = (std::max)(canvasSize.x, 320.0F);
    canvasSize.y = (std::max)(canvasSize.y, 260.0F);
    ImGui::InvisibleButton("forge_viewport_canvas",
                           canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 canvasMin = ImGui::GetItemRectMin();
    const ImVec2 canvasMax = ImGui::GetItemRectMax();
    const ImVec2 center{(canvasMin.x + canvasMax.x) * 0.5F, (canvasMin.y + canvasMax.y) * 0.5F};
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    draw_viewport_grid(draw, canvasMin, canvasMax, center, state);
    handle_viewport_input(editor, state, canvasMin, canvasMax, center, hovered);
    draw_viewport_objects(editor, draw, center, state);
    if (hovered) {
        ImGui::SetTooltip(state.liveEditsOnly
                              ? "Live edits require a native runtime binding. Right drag pans. "
                                "Mouse wheel zooms."
                              : "Local preview mode. Left click selects and drags. Right drag "
                                "pans. Mouse wheel zooms.");
    }
    ImGui::EndChild();
}

void draw_toolbar(workspace::EditorWorkspace& editor, EditorUiState& state) {
    const project::scene::ForgeObject* const selected = editor.selected_object();
    const bool canAddStatic = can_place_kind(editor, state, core::ObjectKind::staticInstance);
    const bool canAddPattern = can_place_kind(editor, state, core::ObjectKind::patternInstance);
    const bool canAddEntity = can_place_kind(editor, state, core::ObjectKind::entityInstance);
    const bool canDuplicate =
        selected != nullptr
        && (!state.liveEditsOnly || runtime_can_spawn_kind(editor, selected->kind));
    const bool canDelete =
        selected != nullptr
        && (!state.liveEditsOnly
            || (editor.runtime_capabilities().has(runtime::Capability::worldDestroy)
                && editor.runtime_binding(selected->id) != nullptr
                && editor.runtime_binding(selected->id)->handle.is_valid()));

    ImGui::BeginDisabled(state.liveEditsOnly);
    if (ImGui::Button("Add Empty")) {
        create_object_from_toolbar(editor, core::ObjectKind::forgeOnly, "Forge Object");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.liveEditsOnly);
    if (ImGui::Button("Add Folder")) {
        (void)editor.create_folder("Folder");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canAddStatic);
    if (ImGui::Button("Add Static")) {
        create_object_from_toolbar(editor, core::ObjectKind::staticInstance, "Static Instance");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canAddPattern);
    if (ImGui::Button("Add Pattern")) {
        create_object_from_toolbar(editor, core::ObjectKind::patternInstance, "Pattern Instance");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canAddEntity);
    if (ImGui::Button("Add Entity")) {
        create_object_from_toolbar(editor, core::ObjectKind::entityInstance, "Entity Instance");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canDuplicate);
    if (ImGui::Button("Duplicate")) {
        (void)editor.duplicate_selected();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("Delete")) {
        (void)editor.delete_selected();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editor.can_undo());
    if (ImGui::Button("Undo")) {
        (void)editor.undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editor.can_redo());
    if (ImGui::Button("Redo")) {
        (void)editor.redo();
    }
    ImGui::EndDisabled();
}

void draw_placement_palette(workspace::EditorWorkspace& editor, EditorUiState& state) {
    ImGui::TextUnformatted("Place Object");
    ImGui::Separator();
    ImGui::InputText("Name", state.createName, sizeof(state.createName));

    core::ObjectKind kind = kind_from_index(state.createKindIndex);
    if (draw_kind_combo("Kind", kind)) {
        state.createKindIndex = kind_index(kind);
        state.createClassId = default_class_for_kind(kind);
        copy_to_buffer(state.createName, default_name_for_kind(kind));
    }

    if (kind != core::ObjectKind::forgeOnly && kind != core::ObjectKind::folder) {
        ImGui::InputScalar("Class ID",
                           ImGuiDataType_U32,
                           &state.createClassId,
                           nullptr,
                           nullptr,
                           "%08X",
                           ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::InputScalar("Tag Hash",
                           ImGuiDataType_U32,
                           &state.createTagHash,
                           nullptr,
                           nullptr,
                           "%08X",
                           ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::InputScalar("Wide Hash",
                           ImGuiDataType_U64,
                           &state.createWideHash,
                           nullptr,
                           nullptr,
                           "%016llX",
                           ImGuiInputTextFlags_CharsHexadecimal);
    }

    ImGui::Checkbox("Parent under selection", &state.parentNewObjectsToSelection);
    const project::scene::ForgeObject* const selected = editor.selected_object();
    core::ForgeUUID parent{};
    if (state.parentNewObjectsToSelection && selected != nullptr) {
        parent = selected->id;
    }

    const bool canPlace = can_place_kind(editor, state, kind);
    ImGui::BeginDisabled(!canPlace);
    if (ImGui::Button("Place", ImVec2(110.0F, 0.0F))) {
        const std::string_view typedName{state.createName};
        (void)editor.create_object(typedName.empty() ? std::string{default_name_for_kind(kind)}
                                                     : std::string{typedName},
                                   kind,
                                   create_resource_from_state(state, kind),
                                   default_transform(),
                                   parent);
    }
    ImGui::EndDisabled();
    if (!canPlace) {
        ImGui::TextWrapped("Live placement is unavailable for this object kind.");
    }
}

void sync_rename_buffer(EditorUiState& state,
                        const project::scene::ForgeObject* selected) noexcept {
    if (selected == nullptr) {
        state.renameObject = {};
        state.renameName[0] = '\0';
        return;
    }
    if (!(state.renameObject == selected->id)) {
        state.renameObject = selected->id;
        copy_to_buffer(state.renameName, selected->editorName);
    }
}

void draw_parent_combo(workspace::EditorWorkspace& editor,
                       const project::scene::ForgeObject& selected) {
    const std::span<const project::scene::ForgeObject> objects = editor.objects();
    const project::scene::ForgeObject* const parent = find_object(objects, selected.parent);
    const std::string_view preview =
        parent == nullptr
            ? "Scene root"
            : (parent->editorName.empty() ? "Unnamed" : std::string_view{parent->editorName});
    if (ImGui::BeginCombo("Parent", preview.data())) {
        if (ImGui::Selectable("Scene root", selected.parent.is_nil())) {
            (void)editor.reparent_object(selected.id, {});
        }
        for (const project::scene::ForgeObject& object : objects) {
            if (object.id == selected.id) {
                continue;
            }
            const std::string_view name =
                object.editorName.empty() ? "Unnamed" : std::string_view{object.editorName};
            std::array<char, 160> label{};
            (void)std::snprintf(label.data(),
                                label.size(),
                                "%.*s  [%.*s]",
                                static_cast<int>(name.size()),
                                name.data(),
                                static_cast<int>(kind_text(object.kind).size()),
                                kind_text(object.kind).data());
            if (ImGui::Selectable(label.data(), object.id == selected.parent)) {
                (void)editor.reparent_object(selected.id, object.id);
            }
        }
        ImGui::EndCombo();
    }
}

void draw_transform_inspector(workspace::EditorWorkspace& editor,
                              EditorUiState& state,
                              const project::scene::ForgeObject& selected) {
    const bool editable = can_edit_transform(editor, state, selected);
    ImGui::BeginDisabled(!editable);
    core::Transform transform = selected.transform;
    float translation[3]{transform.translation.x, transform.translation.y, transform.translation.z};
    if (ImGui::InputFloat3("Position", translation, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        transform.translation.x = translation[0];
        transform.translation.y = translation[1];
        transform.translation.z = translation[2];
        (void)editor.set_selected_transform(transform);
    }

    float rotation[4]{
        transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w};
    if (ImGui::InputFloat4(
            "Rotation quat", rotation, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        transform.rotation.x = rotation[0];
        transform.rotation.y = rotation[1];
        transform.rotation.z = rotation[2];
        transform.rotation.w = rotation[3];
        (void)editor.set_selected_transform(transform);
    }

    float scale = transform.uniformScale;
    if (ImGui::InputFloat(
            "Uniform scale", &scale, 0.1F, 1.0F, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        transform.uniformScale = (std::max)(scale, 0.01F);
        (void)editor.set_selected_transform(transform);
    }

    if (ImGui::Button("X -1")) {
        transform.translation.x -= 1.0F;
        (void)editor.set_selected_transform(transform);
    }
    ImGui::SameLine();
    if (ImGui::Button("X +1")) {
        transform.translation.x += 1.0F;
        (void)editor.set_selected_transform(transform);
    }
    ImGui::SameLine();
    if (ImGui::Button("Z -1")) {
        transform.translation.z -= 1.0F;
        (void)editor.set_selected_transform(transform);
    }
    ImGui::SameLine();
    if (ImGui::Button("Z +1")) {
        transform.translation.z += 1.0F;
        (void)editor.set_selected_transform(transform);
    }
    if (ImGui::Button("Drop to Y=0")) {
        transform.translation.y = 0.0F;
        (void)editor.set_selected_transform(transform);
    }
    ImGui::EndDisabled();
    if (!editable) {
        ImGui::TextWrapped("Transform writes require a live runtime object binding.");
    }
}

void draw_resource_inspector(workspace::EditorWorkspace& editor,
                             const project::scene::ForgeObject& selected) {
    if (selected.kind == core::ObjectKind::forgeOnly || selected.kind == core::ObjectKind::folder) {
        return;
    }

    core::ResourceId resource = selected.resource;
    bool changed = false;
    changed = ImGui::InputScalar("Class ID",
                                 ImGuiDataType_U32,
                                 &resource.classId,
                                 nullptr,
                                 nullptr,
                                 "%08X",
                                 ImGuiInputTextFlags_CharsHexadecimal)
              || changed;
    changed = ImGui::InputScalar("Tag Hash",
                                 ImGuiDataType_U32,
                                 &resource.tagHash,
                                 nullptr,
                                 nullptr,
                                 "%08X",
                                 ImGuiInputTextFlags_CharsHexadecimal)
              || changed;
    changed = ImGui::InputScalar("Wide Hash",
                                 ImGuiDataType_U64,
                                 &resource.wideHash,
                                 nullptr,
                                 nullptr,
                                 "%016llX",
                                 ImGuiInputTextFlags_CharsHexadecimal)
              || changed;
    if (changed) {
        (void)editor.set_selected_resource(resource);
    }

    const std::array<char, 112> resourceLabel = resource_text(selected.resource);
    ImGui::Text("Resource: %s", resourceLabel.data());
}

void draw_runtime_inspector(workspace::EditorWorkspace& editor,
                            const project::scene::ForgeObject& selected) {
    const workspace::ObjectRuntimeBinding* const binding = editor.runtime_binding(selected.id);
    const std::string_view label = runtime_label(selected, binding);
    ImGui::Text("Runtime: %.*s", static_cast<int>(label.size()), label.data());
    if (binding == nullptr) {
        return;
    }

    const std::string_view status = runtime::status_text(binding->lastStatus);
    ImGui::Text("Runtime status: %.*s", static_cast<int>(status.size()), status.data());
    ImGui::Text("Runtime handle: 0x%llX", static_cast<unsigned long long>(binding->handle.value));
    if (!binding->lastAction.empty()) {
        ImGui::Text("Runtime action: %s", binding->lastAction.c_str());
    }
    if (!binding->lastDetail.empty()) {
        ImGui::Text("Runtime detail: %s", binding->lastDetail.c_str());
    }
}

void draw_inspector(workspace::EditorWorkspace& editor, EditorUiState& state) {
    ImGui::TextUnformatted("Inspector");
    ImGui::Separator();
    const project::scene::ForgeObject* selected = editor.selected_object();
    sync_rename_buffer(state, selected);
    if (selected == nullptr) {
        ImGui::TextUnformatted("Nothing selected");
        return;
    }

    ImGui::BeginDisabled(selected->editorLocked);
    const bool renameSubmitted = ImGui::InputText(
        "Name", state.renameName, sizeof(state.renameName), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool renameClicked = ImGui::Button("Apply");
    if (renameSubmitted || renameClicked) {
        (void)editor.rename_selected(state.renameName);
    }

    core::ObjectKind kind = selected->kind;
    if (draw_kind_combo("Kind", kind)) {
        (void)editor.set_selected_kind(kind);
    }
    ImGui::EndDisabled();

    bool visible = selected->editorVisible;
    bool locked = selected->editorLocked;
    bool flagsChanged = ImGui::Checkbox("Visible", &visible);
    ImGui::SameLine();
    flagsChanged = ImGui::Checkbox("Locked", &locked) || flagsChanged;
    if (flagsChanged) {
        (void)editor.set_selected_editor_flags(visible, locked);
    }

    draw_parent_combo(editor, *selected);
    ImGui::Spacing();
    ImGui::Text("Kind: %s", kind_text(selected->kind).data());
    const std::array<char, 17> id = uuid_suffix(selected->id);
    ImGui::Text("Forge ID: %s", id.data());
    ImGui::Text("Scripts: %zu", selected->scripts.size());
    ImGui::Text("Native overrides: %zu", selected->nativeOverrides.size());

    ImGui::SeparatorText("Runtime");
    draw_runtime_inspector(editor, *selected);

    ImGui::SeparatorText("Transform");
    ImGui::BeginDisabled(selected->editorLocked);
    draw_transform_inspector(editor, state, *selected);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Resource");
    ImGui::BeginDisabled(selected->editorLocked);
    draw_resource_inspector(editor, *selected);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginDisabled(selected->editorLocked);
    if (ImGui::Button("Duplicate")) {
        (void)editor.duplicate_selected();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Selected")) {
        (void)editor.delete_selected();
    }
    ImGui::EndDisabled();
}

void draw_runtime_bridge(workspace::EditorWorkspace& editor, EditorUiState& state) {
    const workspace::BaseplateTemplate& current = editor.active_template();
    const bool packageAuthoring = current.id == std::string_view{"blank_baseplate"};
    const runtime::CapabilitySet capabilities = editor.runtime_capabilities();
    const runtime::WorldContext world = editor.runtime_world();
    ImGui::SeparatorText("Destiny Bridge");
    ImGui::Text("Edit mode: %.*s",
                static_cast<int>(edit_mode_text(state).size()),
                edit_mode_text(state).data());
    ImGui::Text("Target: %s", current.destinationHint.data());
    ImGui::SameLine();
    ImGui::Text("Bubble: %s", current.bubbleHint.data());
    ImGui::SameLine();
    ImGui::Text("Redirect: %s", current.hasLaunchTarget ? "available" : "not validated");
    ImGui::Text("World: destination %u / bubble %u", world.destination.value, world.bubble.value);
    ImGui::Text("Object runtime: static %s, pattern %s, transform write %s",
                capabilities.has(runtime::Capability::worldStaticSpawn) ? "yes" : "no",
                capabilities.has(runtime::Capability::worldPatternSpawn) ? "yes" : "no",
                capabilities.has(runtime::Capability::worldTransformWrite) ? "yes" : "no");

    ImGui::BeginDisabled(!current.hasLaunchTarget && !packageAuthoring);
    if (ImGui::Button(packageAuthoring ? "Build Map Package" : "Launch In Destiny")) {
        (void)editor.launch_selected_template();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!current.hasLaunchTarget);
    if (ImGui::Button("Arm Redirect")) {
        (void)editor.arm_selected_template_redirect();
    }
    ImGui::SameLine();
    if (ImGui::Button("Probe Direct Launch")) {
        (void)editor.probe_selected_template_launch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Arm + Open Director")) {
        (void)editor.arm_selected_template_redirect();
        (void)editor.request_native_director_handoff();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Fly/Noclip")) {
        (void)editor.enter_anchored_navigation();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Redirect")) {
        editor.clear_destination_redirect();
    }

    if (!editor.last_launch_message().empty()) {
        ImGui::TextWrapped("%.*s",
                           static_cast<int>(editor.last_launch_message().size()),
                           editor.last_launch_message().data());
    }
    if (!editor.last_runtime_message().empty()) {
        ImGui::TextWrapped("Runtime: %.*s",
                           static_cast<int>(editor.last_runtime_message().size()),
                           editor.last_runtime_message().data());
    }
}

void draw_workspace(workspace::EditorWorkspace& editor) {
    EditorUiState& state = ui_state();
    ImGui::TextUnformatted("Forge Workspace");
    ImGui::Separator();
    ImGui::Text("Template: %s", editor.active_template().displayName.data());
    ImGui::SameLine();
    ImGui::Text("Objects: %zu", editor.objects().size());
    ImGui::SameLine();
    ImGui::Text("Undo: %zu / Redo: %zu", editor.undo_count(), editor.redo_count());

    draw_toolbar(editor, state);
    ImGui::SameLine();
    if (ImGui::Button("Return to Launcher")) {
        editor.return_to_launcher();
        return;
    }
    ImGui::Spacing();
    draw_runtime_bridge(editor, state);
    ImGui::Spacing();

    if (ImGui::BeginTable(
            "izanami_workspace", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthStretch, 0.24F);
        ImGui::TableSetupColumn("Viewport", ImGuiTableColumnFlags_WidthStretch, 0.48F);
        ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch, 0.28F);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("izanami_hierarchy_panel", ImVec2(0.0F, 0.0F), false);
        draw_hierarchy(editor, state);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("izanami_viewport_panel", ImVec2(0.0F, 0.0F), false);
        draw_viewport(editor, state);
        draw_placement_palette(editor, state);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginChild("izanami_inspector_panel", ImVec2(0.0F, 0.0F), false);
        draw_inspector(editor, state);
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void draw_status() noexcept {
    kernel::Kernel& state = kernel::kernel();
    state.initialize_defaults_once();
    ImGui::Text("Services: %zu", state.services().count());
    ImGui::Text("Components: %zu", state.components().count());
    ImGui::Text("Events published: %zu", state.events().published_count());
    ImGui::Text("Project schema: %u", serialization::kProjectFormatVersion);
    ImGui::Text("Scene schema: %u", serialization::kSceneSchemaVersion);
    ImGui::Text("Fate ABI: %u", serialization::kFateModuleAbiVersion);

    const runtime::CapabilitySet capabilities = workspace::workspace().runtime_capabilities();
    ImGui::Separator();
    for (const CapabilityRow& row : kCapabilityRows) {
        ImGui::BulletText(
            "%s: %s", row.label.data(), capabilities.has(row.capability) ? "yes" : "no");
    }
}

void draw_research() noexcept {
    for (const research::NativeTargetRecord& target : research::native_target_records()) {
        ImGui::BulletText("%s: %s", target.capability.data(), evidence_text(target.status).data());
    }
}

void draw_fate() {
    lexer::Lexer scanner{kSampleFate};
    std::vector<lexer::Token> tokens;
    for (;;) {
        lexer::Token token = scanner.next();
        const bool done = token.kind == lexer::TokenKind::endOfFile;
        tokens.push_back(token);
        if (done) {
            break;
        }
    }
    parser::Parser sampleParser{tokens};
    const parser::ParseResult parsed = sampleParser.parse_module();
    ImGui::Text("Lexer tokens: %zu", tokens.size());
    ImGui::Text("Parsed entities: %zu", parsed.module.entities.size());
    ImGui::Text("Parser status: %s", parsed.ok ? "ok" : "error");
}

} // namespace

/** Draws the Izanami Forge launcher/workspace page inside Sunrise's selected UI module frame. */
void draw() noexcept {
    workspace::EditorWorkspace& editor = workspace::workspace();
    editor.initialize_defaults_once();

    ImGui::TextUnformatted("Izanami Forge");
    ImGui::TextWrapped("Open a Forge authoring workspace from a baseplate or bubble template.");
    ImGui::Spacing();

    if (editor.session_state() == workspace::SessionState::launcher) {
        draw_launcher(editor);
    } else {
        draw_workspace(editor);
    }

    if (ImGui::CollapsingHeader("Status")) {
        draw_status();
    }
    if (ImGui::CollapsingHeader("Research")) {
        draw_research();
    }
    if (ImGui::CollapsingHeader("Fate")) {
        draw_fate();
    }
}

/** Draws the standalone in-game Forge overlay outside the Sunrise module browser. */
bool draw_standalone() noexcept {
    if (!standalone_visible()) {
        return false;
    }

    ImGuiViewport* const viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0F || viewport->Size.y <= 0.0F) {
        return false;
    }

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
    ImGui::SetNextWindowSize(
        {viewport->Size.x * kStandaloneViewportScale, viewport->Size.y * kStandaloneViewportScale},
        ImGuiCond_Appearing);

    bool open = true;
    const bool submitContents =
        ImGui::Begin("Izanami Forge##standalone", &open, kStandaloneWindowFlags);
    if (submitContents) {
        draw();
    }
    ImGui::End();

    if (!open) {
        (void)set_standalone_visible(false);
    }
    return true;
}

/** @return True while the standalone Forge overlay should capture input. */
bool standalone_visible() noexcept {
    return g_standaloneVisible.load(std::memory_order_acquire);
}

/** Sets standalone overlay visibility directly. */
bool set_standalone_visible(bool visible) noexcept {
    const bool prior = g_standaloneVisible.exchange(visible, std::memory_order_acq_rel);
    if (prior != visible) {
        report_overlay_visibility(visible);
    }
    return true;
}

/** Handles the standalone Forge overlay hotkey. */
bool toggle_standalone_for_key(UINT virtualKey) noexcept {
    if (virtualKey != kStandaloneToggleKey) {
        return false;
    }
    const bool next = !standalone_visible();
    (void)set_standalone_visible(next);
    if (next) {
        (void)::sunrise::core::ui::runtime::set_visible(false);
    }
    return true;
}

/** @return Windows virtual key used for direct Forge access. */
UINT standalone_toggle_key() noexcept {
    return kStandaloneToggleKey;
}

} // namespace sunrise::izanami::editor::ui
