#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../core/ids.h"
#include "../../project/scene/forge_object.h"
#include "../../project/scene/scene_document.h"
#include "../../runtime/runtime_adapter.h"
#include "../commands/command_queue.h"

namespace sunrise::izanami::editor::workspace {

enum class SessionState : std::uint8_t {
    launcher,
    activeWorkspace,
};

struct BaseplateTemplate {
    std::string_view id{};
    std::string_view displayName{};
    std::string_view destinationHint{};
    std::string_view bubbleHint{};
    std::string_view description{};
    std::string_view packageName{};
    std::uint8_t bubble{};
    std::uint16_t sliceSet{};
    std::uint32_t spawnSetHash{};
    bool hasLaunchTarget{};
    bool hasSpawnSet{};
    bool resolveFromScenarioCatalog{};
};

struct LaunchResult {
    bool workspaceStarted{};
    bool destinationTransitionStarted{};
    bool forcedDestinationArmed{};
    bool directLaunchProbePrepared{};
    bool anchoredNavigationEnabled{};
    bool nativeActivityLaunchRequested{};
    bool nativeDirectorHandoffRequested{};
    bool uiHidden{};
    std::string_view message{};
};

struct ObjectRuntimeBinding {
    core::ForgeUUID object{};
    runtime::ForgeHandle handle{};
    runtime::RuntimeStatus lastStatus{runtime::RuntimeStatus::unsupported};
    std::string lastAction{};
    std::string lastDetail{};
};

class EditorWorkspace final {
public:
    void initialize_defaults_once();
    void reset();

    [[nodiscard]] std::span<const BaseplateTemplate> templates() const noexcept;
    [[nodiscard]] std::size_t selected_template_index() const noexcept;
    [[nodiscard]] bool select_template(std::size_t index) noexcept;
    [[nodiscard]] LaunchResult open_selected_template();
    [[nodiscard]] LaunchResult launch_selected_template();
    [[nodiscard]] LaunchResult probe_selected_template_launch();
    [[nodiscard]] LaunchResult arm_selected_template_redirect();
    [[nodiscard]] LaunchResult request_native_director_handoff();
    [[nodiscard]] LaunchResult enter_anchored_navigation();
    void clear_destination_redirect();
    void return_to_launcher();

    [[nodiscard]] core::ForgeUUID create_forge_object(std::string name);
    [[nodiscard]] core::ForgeUUID create_object(std::string name,
                                                core::ObjectKind kind,
                                                core::ResourceId resource,
                                                core::Transform transform,
                                                core::ForgeUUID parent = {});
    [[nodiscard]] core::ForgeUUID create_folder(std::string name);
    [[nodiscard]] core::ForgeUUID duplicate_selected();
    [[nodiscard]] bool delete_selected();
    [[nodiscard]] bool rename_selected(std::string name);
    [[nodiscard]] bool set_selected_kind(core::ObjectKind kind);
    [[nodiscard]] bool set_selected_resource(core::ResourceId resource);
    [[nodiscard]] bool set_selected_editor_flags(bool visible, bool locked);
    [[nodiscard]] bool reparent_object(core::ForgeUUID id, core::ForgeUUID parent);
    [[nodiscard]] bool select(core::ForgeUUID id) noexcept;
    [[nodiscard]] bool set_selected_transform(core::Transform transform);
    [[nodiscard]] bool preview_selected_transform(core::Transform transform) noexcept;
    [[nodiscard]] bool commit_selected_transform(core::Transform before, core::Transform after);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();

    [[nodiscard]] core::ForgeUUID selected_id() const noexcept;
    [[nodiscard]] const project::scene::ForgeObject* selected_object() const noexcept;
    [[nodiscard]] const project::scene::ForgeObject* find_object(core::ForgeUUID id) const noexcept;
    [[nodiscard]] std::span<const project::scene::ForgeObject> objects() const noexcept;
    [[nodiscard]] SessionState session_state() const noexcept;
    [[nodiscard]] const BaseplateTemplate& active_template() const noexcept;
    [[nodiscard]] std::string_view last_launch_message() const noexcept;
    [[nodiscard]] std::size_t command_count() const noexcept;
    [[nodiscard]] std::size_t undo_count() const noexcept;
    [[nodiscard]] std::size_t redo_count() const noexcept;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] const ObjectRuntimeBinding* runtime_binding(core::ForgeUUID id) const noexcept;
    [[nodiscard]] runtime::CapabilitySet runtime_capabilities() const noexcept;
    [[nodiscard]] runtime::WorldContext runtime_world() const noexcept;
    [[nodiscard]] std::string_view last_runtime_message() const noexcept;

private:
    [[nodiscard]] bool apply(commands::Command command);
    [[nodiscard]] bool apply_to_scene(const commands::Command& command);
    void apply_to_runtime(const commands::Command& command);
    [[nodiscard]] bool make_inverse(const commands::Command& command,
                                    commands::Command& inverse) const;
    [[nodiscard]] bool apply_transaction(const std::vector<commands::Command>& commands);
    void commit_transaction(commands::Command redo, commands::Command undo, std::string label);
    void bind_runtime_object(const project::scene::ForgeObject& object, std::string_view reason);
    void destroy_runtime_object(core::ForgeUUID id, std::string_view reason);
    void transform_runtime_object(const project::scene::ForgeObject& object,
                                  std::string_view reason);
    [[nodiscard]] ObjectRuntimeBinding& runtime_binding_for(core::ForgeUUID id);
    void remove_runtime_binding(core::ForgeUUID id);
    void record_runtime(core::ForgeUUID id,
                        std::string_view action,
                        runtime::RuntimeStatus status,
                        runtime::ForgeHandle handle,
                        std::string_view detail);
    [[nodiscard]] core::ForgeUUID next_uuid() noexcept;

    project::scene::SceneDocument scene_{};
    commands::CommandQueue queue_{};
    commands::TransactionHistory history_{};
    core::ForgeUUID selected_{};
    std::uint64_t nextId_{1};
    std::size_t selectedTemplate_{};
    std::size_t activeTemplate_{};
    SessionState sessionState_{SessionState::launcher};
    std::string_view lastLaunchMessage_{};
    std::string lastGameplayModeMessage_{};
    std::string lastRuntimeMessage_{};
    std::vector<ObjectRuntimeBinding> runtimeBindings_{};
    bool initialized_{};
};

[[nodiscard]] EditorWorkspace& workspace() noexcept;

} // namespace sunrise::izanami::editor::workspace
