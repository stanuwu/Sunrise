#include "editor_workspace.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "../../../core/logging/log.h"
#include "../../../server/bap/encrypted/activity_host_manager/activity_host_manager_route.h"
#include "../../../state/activity/forced/activity_forced_destination.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/scenarios/definition.h"
#include "../../runtime/baseplate_composition.h"
#include "../../runtime/gameplay_editor_mode.h"

namespace sunrise::izanami::editor::workspace {

namespace {

constexpr std::uint64_t kIzanamiUuidDomain = 0x495A414E414D4930ULL;

constexpr std::array<BaseplateTemplate, 6> kTemplates{{
    {"blank_baseplate",
     "Blank Baseplate",
     "Izanami Baseplate",
     "custom package required",
     "Targets a scenery-free Izanami map package. Stock Destiny destinations are deliberately "
     "disabled for this template until that package has been generated and validated.",
     "",
     0,
     0,
     0,
     false,
     false,
     false},
    {"tower_carrier_control",
     "Tower Carrier Control",
     "activity index 20 fallback",
     "forced redirect disabled",
     "Controlled direct-launch test for the early local carrier. It clears every forced package "
     "override before requesting Destiny's native activity-session transition.",
     "",
     0,
     0,
     0,
     false,
     false,
     false},
    {"vfx_shade_test_template",
     "VFX Shade Test Baseplate",
     "vfx_shade_test",
     "bubble 0 / slice 0",
     "Dirty installed test scenario kept as a proof target. It can show leftover environment "
     "pieces, so Blank Baseplate no longer prefers it.",
     "vfx_shade_test",
     0,
     0,
     0,
     true,
     false,
     false},
    {"current_bubble_template",
     "Current Bubble Template",
     "current destination",
     "current bubble",
     "Requires world-context binding so Izanami can read the current package, bubble, and slice "
     "set safely.",
     "",
     0,
     0,
     0,
     false,
     false,
     false},
    {"social_space_template",
     "Tower Redirect Test",
     "city_tower_social_d2",
     "bubble 0 / slice 48",
     "Experimental redirect test that intentionally lands in the normal Tower social space. This "
     "is not a Forge baseplate.",
     "city_tower_social_d2",
     0,
     48,
     state::activity::forced::kAbsentSpawnSetHash,
     true,
     false,
     false},
    {"encounter_sandbox",
     "Encounter Sandbox",
     "activity destination",
     "combat bubble",
     "Requires a validated combat destination package, bubble, slice set, and spawn set before "
     "Izanami can arm it safely.",
     "",
     0,
     0,
     0,
     false,
     false,
     false},
}};

EditorWorkspace g_workspace;

struct ResolvedDestinationTarget {
    std::array<char, state::activity::destination::kPackageNameCapacity> packageName{};
    std::size_t packageNameLength{};
    std::uint8_t bubble{};
    std::uint16_t sliceSet{};
    std::uint32_t spawnSetHash{state::activity::forced::kAbsentSpawnSetHash};
    bool hasSpawnSet{};
    bool fromCatalog{};
};

struct DirectLaunchProbe {
    std::uint64_t sessionId{};
    std::size_t responseBytes{};
    bool encoded{};
    bool prepared{};
};

[[nodiscard]] bool same_transform(core::Transform left, core::Transform right) noexcept {
    return left.translation.x == right.translation.x && left.translation.y == right.translation.y
           && left.translation.z == right.translation.z && left.rotation.x == right.rotation.x
           && left.rotation.y == right.rotation.y && left.rotation.z == right.rotation.z
           && left.rotation.w == right.rotation.w && left.uniformScale == right.uniformScale;
}

[[nodiscard]] std::string default_name_for_kind(core::ObjectKind kind) {
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

[[nodiscard]] std::string label_for_command(commands::CommandKind kind) {
    switch (kind) {
    case commands::CommandKind::createObject:
        return "Create object";
    case commands::CommandKind::deleteObject:
        return "Delete object";
    case commands::CommandKind::setTransform:
        return "Set transform";
    case commands::CommandKind::renameObject:
        return "Rename object";
    case commands::CommandKind::setObjectKind:
        return "Set object kind";
    case commands::CommandKind::setResource:
        return "Set resource";
    case commands::CommandKind::setEditorFlags:
        return "Set editor flags";
    case commands::CommandKind::reparentObject:
        return "Reparent object";
    }
    return "Edit object";
}

[[nodiscard]] std::string_view
scenario_name(const state::build_data::scenarios::Definition& definition) noexcept {
    return {definition.name.data(), definition.nameLength};
}

[[nodiscard]] std::string_view target_name(const ResolvedDestinationTarget& target) noexcept {
    return {target.packageName.data(), target.packageNameLength};
}

[[nodiscard]] std::size_t
declared_bubble_count(const state::build_data::scenarios::Definition& definition) noexcept {
    return (std::min)(static_cast<std::size_t>(definition.bubbleCount),
                      definition.bubbleStates.size());
}

[[nodiscard]] std::size_t
first_live_bubble(const state::build_data::scenarios::Definition& definition) noexcept {
    const std::size_t count = declared_bubble_count(definition);
    for (std::size_t bubble = 0; bubble < count; ++bubble) {
        if (definition.bubbleStates[bubble] == state::build_data::scenarios::kBubbleEnabledByte) {
            return bubble;
        }
    }
    return static_cast<std::size_t>(-1);
}

[[nodiscard]] bool write_target_name(ResolvedDestinationTarget& target,
                                     std::string_view name) noexcept {
    if (name.empty() || name.size() > target.packageName.size()) {
        return false;
    }
    std::copy_n(name.begin(), name.size(), target.packageName.begin());
    target.packageNameLength = name.size();
    return true;
}

[[nodiscard]] bool target_from_scenario(const state::build_data::scenarios::Definition& definition,
                                        ResolvedDestinationTarget& target) noexcept {
    target = {};
    const std::string_view name = scenario_name(definition);
    if (!write_target_name(target, name)) {
        return false;
    }
    const std::size_t bubble = first_live_bubble(definition);
    if (bubble == static_cast<std::size_t>(-1) || bubble > 0xFFU) {
        return false;
    }
    target.bubble = static_cast<std::uint8_t>(bubble);
    target.sliceSet = static_cast<std::uint16_t>(bubble * 8U);
    target.spawnSetHash = state::activity::forced::kAbsentSpawnSetHash;
    target.hasSpawnSet = false;
    target.fromCatalog = true;
    return true;
}

[[nodiscard]] bool find_scenario_target(std::string_view name,
                                        ResolvedDestinationTarget& target) noexcept {
    state::build_data::scenarios::Definition definition{};
    return state::build_data::find_scenario_layout(name, definition)
           && target_from_scenario(definition, target);
}

[[nodiscard]] bool
is_minimal_empty_ambient(const state::build_data::scenarios::Definition& definition) noexcept {
    const std::string_view name = scenario_name(definition);
    return name.rfind("empty_ambient_", 0) == 0 && declared_bubble_count(definition) == 1
           && definition.rosterGroupCount <= 1 && definition.bubbleStateCounts[0] == 1
           && first_live_bubble(definition) == 0;
}

[[nodiscard]] bool resolve_catalog_baseplate_target(ResolvedDestinationTarget& target) noexcept {
    constexpr std::array<std::string_view, 14> kPreferredNames{
        "empty_ambient_trophy_hall",
        "empty_ambient_pvp_anomaly_2",
        "empty_ambient_pvp_echo",
        "empty_ambient_pvp_fort",
        "empty_ambient_pvp_peak",
        "empty_ambient_dungeon_prophecy",
        "empty_ambient_pandora",
        "if_test_easy",
        "if_test_solo_race_specific",
        "vfx_shade_test",
        "trials_social_space_d2",
        "d2_campaign_social_space",
        "city_tower_social_d2",
        "dreaming_city_freeroam",
    };

    for (const std::string_view name : kPreferredNames) {
        if (find_scenario_target(name, target)) {
            return true;
        }
    }

    std::array<state::build_data::scenarios::Definition,
               state::build_data::scenarios::kDefinitionCapacity>
        definitions{};
    std::size_t count = 0;
    if (!state::build_data::snapshot_scenario_layouts(definitions, count)) {
        return false;
    }
    for (const state::build_data::scenarios::Definition& definition :
         std::span(definitions).first(count)) {
        if (is_minimal_empty_ambient(definition) && target_from_scenario(definition, target)) {
            return true;
        }
    }
    for (const state::build_data::scenarios::Definition& definition :
         std::span(definitions).first(count)) {
        const std::string_view name = scenario_name(definition);
        if (name.find("empty_") != std::string_view::npos
            && target_from_scenario(definition, target)) {
            return true;
        }
    }
    for (const state::build_data::scenarios::Definition& definition :
         std::span(definitions).first(count)) {
        if (definition.rosterGroupCount == 0 && target_from_scenario(definition, target)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool resolve_static_template_target(const BaseplateTemplate& target,
                                                  ResolvedDestinationTarget& resolved) noexcept {
    state::build_data::scenarios::Definition definition{};
    if (state::build_data::find_scenario_layout(target.packageName, definition)
        && target_from_scenario(definition, resolved)) {
        resolved.hasSpawnSet = target.hasSpawnSet;
        resolved.spawnSetHash = target.spawnSetHash;
        return true;
    }

    resolved = {};
    if (!target.hasLaunchTarget || target.packageName.empty()
        || !write_target_name(resolved, target.packageName)) {
        return false;
    }
    resolved.bubble = target.bubble;
    resolved.sliceSet = target.sliceSet;
    resolved.spawnSetHash = target.spawnSetHash;
    resolved.hasSpawnSet = target.hasSpawnSet;
    resolved.fromCatalog = false;
    return true;
}

[[nodiscard]] bool resolve_template_target(const BaseplateTemplate& target,
                                           ResolvedDestinationTarget& resolved) noexcept {
    if (target.resolveFromScenarioCatalog && resolve_catalog_baseplate_target(resolved)) {
        return true;
    }
    return resolve_static_template_target(target, resolved);
}

void report_baseplate_target(std::string_view stage,
                             std::string_view result,
                             const ResolvedDestinationTarget& target) noexcept {
    std::array<char, ::sunrise::core::log::kLineCapacity> line{};
    const std::string_view name = target_name(target);
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=izanami_baseplate stage=%.*s result=%.*s package=%.*s bubble=%u "
                      "slice_set=%u spawn=0x%08X source=%s",
                      static_cast<int>(stage.size()),
                      stage.data(),
                      static_cast<int>(result.size()),
                      result.data(),
                      static_cast<int>(name.size()),
                      name.data(),
                      static_cast<unsigned>(target.bubble),
                      static_cast<unsigned>(target.sliceSet),
                      static_cast<unsigned>(target.spawnSetHash),
                      target.fromCatalog ? "catalog" : "template");
    if (written > 0) {
        ::sunrise::core::log::write(::sunrise::core::log::Channel::client,
                                    result == "ok" ? ::sunrise::core::log::Level::info
                                                   : ::sunrise::core::log::Level::warn,
                                    {line.data(), static_cast<std::size_t>(written)});
    }
}

void report_direct_launch_probe(const DirectLaunchProbe& probe) noexcept {
    std::array<char, ::sunrise::core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=izanami_direct_launch stage=svc6_probe result=%s prepared=%u "
                      "session=0x%llX response_bytes=%zu committed=0",
                      probe.encoded && probe.prepared ? "ok" : "fail",
                      probe.prepared ? 1U : 0U,
                      static_cast<unsigned long long>(probe.sessionId),
                      probe.responseBytes);
    if (written > 0) {
        ::sunrise::core::log::write(::sunrise::core::log::Channel::client,
                                    probe.encoded && probe.prepared
                                        ? ::sunrise::core::log::Level::info
                                        : ::sunrise::core::log::Level::warn,
                                    {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool arm_forced_destination(const BaseplateTemplate& target) noexcept {
    ResolvedDestinationTarget resolved{};
    if (!resolve_template_target(target, resolved)) {
        report_baseplate_target("resolve", "fail", resolved);
        return false;
    }
    state::activity::forced::ForcedDestination forced{};
    const std::size_t length = (std::min)(resolved.packageNameLength, forced.packageName.size());
    std::copy_n(resolved.packageName.begin(), length, forced.packageName.begin());
    forced.packageNameLength = static_cast<std::uint8_t>(length);
    forced.bubble = resolved.bubble;
    forced.sliceSet = resolved.sliceSet;
    forced.spawnSetHash = resolved.spawnSetHash;
    forced.hasBubble = true;
    forced.hasSliceSet = true;
    forced.hasSpawnSetHash = resolved.hasSpawnSet;
    forced.enabled = true;
    const bool published = state::activity::forced::publish(forced);
    report_baseplate_target("arm", published ? "ok" : "fail", resolved);
    return published;
}

[[nodiscard]] DirectLaunchProbe probe_forced_activity_manager_request() noexcept {
    // Mirrors the smallest legal service-6 body: discriminator 3, zero protobuf bytes, padding.
    // The route can rewrite that absent selection through the forced-destination bridge.
    constexpr std::size_t kActivityManagerRequestBytes = 7'719;
    constexpr std::size_t kActivityManagerResponseBytes = 137;
    std::array<std::byte, kActivityManagerRequestBytes> request{};
    std::array<std::byte, kActivityManagerResponseBytes> response{};
    request[0] = std::byte{3};

    DirectLaunchProbe probe{};
    state::activity::PendingAllocation allocation{};
    bool hasAllocation = false;
    probe.encoded = server::bap::encrypted::activity_host_manager::encode_response(
        request, response, probe.responseBytes, allocation, hasAllocation);
    probe.prepared = hasAllocation && allocation.prepared;
    probe.sessionId = allocation.sessionId;
    allocation = {};
    report_direct_launch_probe(probe);
    return probe;
}

[[nodiscard]] runtime::IForgeRuntime& forge_runtime() noexcept {
    return runtime::unsupported_runtime();
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

void report(std::string_view stage,
            std::string_view result,
            std::string_view detail = {}) noexcept {
    std::array<char, ::sunrise::core::log::kLineCapacity> line{};
    const int written = detail.empty()
                            ? std::snprintf(line.data(),
                                            line.size(),
                                            "ev=izanami stage=%.*s result=%.*s",
                                            static_cast<int>(stage.size()),
                                            stage.data(),
                                            static_cast<int>(result.size()),
                                            result.data())
                            : std::snprintf(line.data(),
                                            line.size(),
                                            "ev=izanami stage=%.*s result=%.*s detail=%.*s",
                                            static_cast<int>(stage.size()),
                                            stage.data(),
                                            static_cast<int>(result.size()),
                                            result.data(),
                                            static_cast<int>(detail.size()),
                                            detail.data());
    if (written > 0) {
        ::sunrise::core::log::write(::sunrise::core::log::Channel::client,
                                    ::sunrise::core::log::Level::info,
                                    {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Seeds a small Forge-only authoring scene for the visible editor shell. */
void EditorWorkspace::initialize_defaults_once() {
    if (initialized_) {
        return;
    }
    initialized_ = true;
    return_to_launcher();
}

/** Clears the current authoring scene and command history. */
void EditorWorkspace::reset() {
    scene_.clear();
    queue_.clear();
    history_.clear();
    runtimeBindings_.clear();
    selected_ = {};
    nextId_ = 1;
    lastRuntimeMessage_.clear();
}

std::span<const BaseplateTemplate> EditorWorkspace::templates() const noexcept {
    return kTemplates;
}

std::size_t EditorWorkspace::selected_template_index() const noexcept {
    return selectedTemplate_;
}

bool EditorWorkspace::select_template(std::size_t index) noexcept {
    if (index >= kTemplates.size()) {
        return false;
    }
    selectedTemplate_ = index;
    return true;
}

/** Opens the Forge-owned editor scene for the chosen template without touching live Destiny. */
LaunchResult EditorWorkspace::open_selected_template() {
    if (selectedTemplate_ >= kTemplates.size()) {
        return {.message = "No template selected."};
    }

    runtime::gameplay_editor_mode::leave();
    const BaseplateTemplate& launchTemplate = kTemplates[selectedTemplate_];
    reset();
    activeTemplate_ = selectedTemplate_;
    sessionState_ = SessionState::activeWorkspace;

    core::Transform anchorTransform{};
    anchorTransform.uniformScale = 1.0F;
    const core::ForgeUUID anchor =
        create_object("Baseplate Anchor", core::ObjectKind::forgeOnly, {}, anchorTransform);

    core::Transform patternTransform{};
    patternTransform.translation.x = 4.0F;
    patternTransform.translation.z = 4.0F;
    patternTransform.uniformScale = 1.0F;
    (void)create_object("Local Pattern Marker", core::ObjectKind::forgeOnly, {}, patternTransform);

    core::Transform doorTransform{};
    doorTransform.translation.x = -4.0F;
    doorTransform.translation.z = 2.0F;
    doorTransform.uniformScale = 1.0F;
    (void)create_object("Fate Test Door", core::ObjectKind::forgeOnly, {}, doorTransform);

    history_.clear();
    queue_.clear();
    if (!anchor.is_nil()) {
        (void)select(anchor);
    }

    std::array<char, 256> message{};
    const int written =
        std::snprintf(message.data(),
                      message.size(),
                      "Editor opened for %s. Runtime actions are separate and experimental.",
                      launchTemplate.displayName.data());
    lastGameplayModeMessage_ = written > 0
                                   ? std::string(message.data(), static_cast<std::size_t>(written))
                                   : std::string{"Editor opened."};
    lastLaunchMessage_ = lastGameplayModeMessage_;
    report("open_editor", "ok", launchTemplate.id);
    return {.workspaceStarted = true, .message = lastLaunchMessage_};
}

/** Builds Blank Baseplate's staged package or starts a stock template's native activity. */
LaunchResult EditorWorkspace::launch_selected_template() {
    const std::size_t templateIndex =
        sessionState_ == SessionState::activeWorkspace ? activeTemplate_ : selectedTemplate_;
    if (templateIndex >= kTemplates.size()) {
        lastGameplayModeMessage_ = "No template selected.";
        lastLaunchMessage_ = lastGameplayModeMessage_;
        return {.message = lastLaunchMessage_};
    }

    selectedTemplate_ = templateIndex;
    const BaseplateTemplate& target = kTemplates[templateIndex];
    const bool isolatedBaseplate = target.id == std::string_view{"blank_baseplate"};
    if (isolatedBaseplate) {
        LaunchResult result{};
        if (!runtime::baseplate_composition::arm()) {
            lastGameplayModeMessage_ =
                "Izanami could not inspect Pandora or generate its staged map package.";
            lastLaunchMessage_ = lastGameplayModeMessage_;
            report("package_stage", "fail", target.id);
            result.message = lastLaunchMessage_;
            return result;
        }
        runtime::baseplate_composition::disarm();
        lastGameplayModeMessage_ =
            "Generated and validated w64_pandora_0687_6.pkg.izanami-stage. The staged package "
            "has not been installed or launched.";
        lastLaunchMessage_ = lastGameplayModeMessage_;
        report("package_stage", "ok", target.id);
        result.message = lastLaunchMessage_;
        return result;
    }
    LaunchResult result = open_selected_template();
    runtime::baseplate_composition::disarm();
    const bool carrierControl = target.id == std::string_view{"tower_carrier_control"};
    bool destinationReady = false;
    if (carrierControl) {
        state::activity::forced::clear();
        destinationReady = true;
        report("carrier_control", "forced_destination_cleared", target.id);
    } else {
        destinationReady = arm_forced_destination(target);
    }
    if (!destinationReady) {
        std::array<char, 320> message{};
        const int written =
            std::snprintf(message.data(),
                          message.size(),
                          "%s could not resolve a native Destiny baseplate target yet.",
                          target.displayName.data());
        lastGameplayModeMessage_ =
            written > 0 ? std::string(message.data(), static_cast<std::size_t>(written))
                        : std::string{"Native baseplate target was not resolved."};
        lastLaunchMessage_ = lastGameplayModeMessage_;
        report("launch_native", "fail", target.id);
        result.message = lastLaunchMessage_;
        return result;
    }

    const runtime::gameplay_editor_mode::NativeActivityLaunchResult launch =
        runtime::gameplay_editor_mode::request_native_activity_launch();
    std::array<char, 384> message{};
    const int written = std::snprintf(
        message.data(),
        message.size(),
        launch.requested
            ? (carrierControl ? "%s queued Destiny's native activity-session transition with "
                                "forced package redirection disabled."
                              : "%s queued Destiny's native activity-session transition. No "
                                "Director click is required.")
            : (launch.targetResolved ? "%s is ready, but Destiny is not currently in orbit."
                                     : "%s is ready, but this game build's native activity-launch "
                                       "target was not resolved."),
        target.displayName.data());
    lastGameplayModeMessage_ = written > 0
                                   ? std::string(message.data(), static_cast<std::size_t>(written))
                                   : std::string{"Izanami native baseplate target armed."};
    lastLaunchMessage_ = lastGameplayModeMessage_;
    report("launch_native", launch.requested ? "ok" : "native_request_fail", target.id);
    return {.workspaceStarted = true,
            .destinationTransitionStarted = launch.requested,
            .forcedDestinationArmed = !carrierControl,
            .nativeActivityLaunchRequested = launch.requested,
            .uiHidden = launch.uiHidden,
            .message = lastLaunchMessage_};
}

/** Arms the selected template and validates the server-side activity allocation path only. */
LaunchResult EditorWorkspace::probe_selected_template_launch() {
    const std::size_t templateIndex =
        sessionState_ == SessionState::activeWorkspace ? activeTemplate_ : selectedTemplate_;
    if (templateIndex >= kTemplates.size()) {
        lastGameplayModeMessage_ = "No template selected.";
        lastLaunchMessage_ = lastGameplayModeMessage_;
        return {.message = lastLaunchMessage_};
    }

    const BaseplateTemplate& target = kTemplates[templateIndex];
    const bool armed = arm_forced_destination(target);
    if (!armed) {
        std::array<char, 320> message{};
        const int written =
            std::snprintf(message.data(),
                          message.size(),
                          "%s has no validated Destiny destination target to probe.",
                          target.displayName.data());
        lastGameplayModeMessage_ =
            written > 0 ? std::string(message.data(), static_cast<std::size_t>(written))
                        : std::string{"No direct launch target to probe."};
        lastLaunchMessage_ = lastGameplayModeMessage_;
        report("direct_launch_probe", "arm_fail", target.id);
        return {.message = lastLaunchMessage_};
    }

    const DirectLaunchProbe probe = probe_forced_activity_manager_request();
    std::array<char, 420> message{};
    const int written =
        std::snprintf(message.data(),
                      message.size(),
                      probe.encoded && probe.prepared
                          ? "%s direct-launch probe prepared service-7 session 0x%llX without "
                            "committing it. Server-side allocation is ready; native client task "
                            "injection is still the unresolved step."
                          : "%s direct-launch probe failed before a service-7 session could be "
                            "prepared.",
                      target.displayName.data(),
                      static_cast<unsigned long long>(probe.sessionId));
    lastGameplayModeMessage_ = written > 0
                                   ? std::string(message.data(), static_cast<std::size_t>(written))
                                   : std::string{"Direct launch probe finished."};
    lastLaunchMessage_ = lastGameplayModeMessage_;
    report("direct_launch_probe", probe.encoded && probe.prepared ? "ok" : "fail", target.id);
    return {.forcedDestinationArmed = true,
            .directLaunchProbePrepared = probe.encoded && probe.prepared,
            .message = lastLaunchMessage_};
}

/** Arms the selected template as the next native Destiny activity redirect. */
LaunchResult EditorWorkspace::arm_selected_template_redirect() {
    const std::size_t templateIndex =
        sessionState_ == SessionState::activeWorkspace ? activeTemplate_ : selectedTemplate_;
    if (templateIndex >= kTemplates.size()) {
        lastGameplayModeMessage_ = "No template selected.";
        lastLaunchMessage_ = lastGameplayModeMessage_;
        return {.message = lastLaunchMessage_};
    }

    const BaseplateTemplate& target = kTemplates[templateIndex];
    const bool armed = arm_forced_destination(target);
    std::array<char, 320> message{};
    const int written =
        std::snprintf(message.data(),
                      message.size(),
                      armed ? "Native destination armed for %s. It rewrites the next activity "
                              "request to Izanami's resolved baseplate target."
                            : "%s has no validated Destiny destination target.",
                      target.displayName.data());
    lastGameplayModeMessage_ = written > 0
                                   ? std::string(message.data(), static_cast<std::size_t>(written))
                                   : std::string{"Destination redirect state updated."};
    lastLaunchMessage_ = lastGameplayModeMessage_;
    report("arm_redirect", armed ? "ok" : "fail", target.id);
    return {.forcedDestinationArmed = armed, .message = lastLaunchMessage_};
}

/** Requests Destiny's own Director after hiding Sunrise UI so the game can see the key pulse. */
LaunchResult EditorWorkspace::request_native_director_handoff() {
    const runtime::gameplay_editor_mode::NativeDirectorHandoffResult handoff =
        runtime::gameplay_editor_mode::request_native_director_handoff();
    const char* source = handoff.usedDestinationsTab
                             ? "Destinations"
                             : (handoff.usedDirector ? "Director" : "default Director key");
    std::array<char, 320> message{};
    const int written = std::snprintf(
        message.data(),
        message.size(),
        handoff.requested ? "Native %s handoff requested. Click a real launchable Destiny node; "
                            "Sunrise can only rewrite that request."
                          : "Native Director handoff failed; open the Director manually.",
        source);
    lastGameplayModeMessage_ = written > 0
                                   ? std::string(message.data(), static_cast<std::size_t>(written))
                                   : std::string{"Native Director handoff requested."};
    lastLaunchMessage_ = lastGameplayModeMessage_;
    report("director_handoff", handoff.requested ? "ok" : "fail", source);
    return {.nativeDirectorHandoffRequested = handoff.requested,
            .uiHidden = handoff.uiHidden,
            .message = lastLaunchMessage_};
}

/** Enters the current best live-game editor foothold: Guardian Fly/Noclip navigation. */
LaunchResult EditorWorkspace::enter_anchored_navigation() {
    const runtime::gameplay_editor_mode::ActivationResult gameplayMode =
        runtime::gameplay_editor_mode::enter();
    lastGameplayModeMessage_ =
        gameplayMode.navigationEnabled
            ? "Anchored Fly/Noclip navigation is active. Press Insert to reopen Forge."
            : "Fly/Noclip navigation could not be published.";
    lastLaunchMessage_ = lastGameplayModeMessage_;
    report("anchored_navigation", gameplayMode.navigationEnabled ? "ok" : "fail");
    return {.workspaceStarted = true,
            .destinationTransitionStarted = false,
            .anchoredNavigationEnabled = gameplayMode.navigationEnabled,
            .uiHidden = gameplayMode.uiHidden,
            .message = lastLaunchMessage_};
}

/** Clears the forced destination so ordinary native activity launches stop being rewritten. */
void EditorWorkspace::clear_destination_redirect() {
    state::activity::forced::clear();
    lastGameplayModeMessage_ = "Destination redirect cleared.";
    lastLaunchMessage_ = lastGameplayModeMessage_;
    report("clear_redirect", "ok");
}

/** Leaves the editor workspace and returns to the project/template launcher. */
void EditorWorkspace::return_to_launcher() {
    runtime::gameplay_editor_mode::leave();
    runtime::baseplate_composition::disarm();
    state::activity::forced::clear();
    reset();
    sessionState_ = SessionState::launcher;
    lastLaunchMessage_ = {};
    lastGameplayModeMessage_ = {};
    report("return_launcher", "ok");
}

/** Creates one Forge-only logical object through the command path. */
core::ForgeUUID EditorWorkspace::create_forge_object(std::string name) {
    core::Transform transform{};
    transform.uniformScale = 1.0F;
    return create_object(std::move(name), core::ObjectKind::forgeOnly, {}, transform);
}

/** Creates one typed logical object through the command path. */
core::ForgeUUID EditorWorkspace::create_object(std::string name,
                                               core::ObjectKind kind,
                                               core::ResourceId resource,
                                               core::Transform transform,
                                               core::ForgeUUID parent) {
    const core::ForgeUUID id = next_uuid();
    commands::Command command;
    command.kind = commands::CommandKind::createObject;
    command.object = id;
    command.objectKind = kind;
    command.resource = resource;
    command.parent = parent;
    command.editorName = std::move(name);
    if (command.editorName.empty()) {
        command.editorName = default_name_for_kind(kind);
    }
    command.transform = transform;
    if (command.transform.uniformScale <= 0.0F) {
        command.transform.uniformScale = 1.0F;
    }
    if (!apply(std::move(command))) {
        return {};
    }
    return id;
}

/** Creates an editor-only hierarchy folder. */
core::ForgeUUID EditorWorkspace::create_folder(std::string name) {
    core::Transform transform{};
    transform.uniformScale = 1.0F;
    return create_object(std::move(name), core::ObjectKind::folder, {}, transform);
}

/** Duplicates the selected logical object and selects the copy. */
core::ForgeUUID EditorWorkspace::duplicate_selected() {
    const project::scene::ForgeObject* const source = selected_object();
    if (source == nullptr) {
        return {};
    }

    project::scene::ForgeObject duplicate = *source;
    duplicate.id = next_uuid();
    duplicate.editorName = duplicate.editorName.empty() ? "Copy" : duplicate.editorName + " Copy";
    if (duplicate.kind != core::ObjectKind::folder) {
        duplicate.transform.translation.x += 1.0F;
        duplicate.transform.translation.z += 1.0F;
    }

    commands::Command command;
    command.kind = commands::CommandKind::createObject;
    command.object = duplicate.id;
    command.objectSnapshot = std::move(duplicate);
    if (!apply(std::move(command))) {
        return {};
    }
    return selected_;
}

/** Deletes the selected object through the command path. */
bool EditorWorkspace::delete_selected() {
    if (selected_.is_nil()) {
        return false;
    }
    commands::Command command;
    command.kind = commands::CommandKind::deleteObject;
    command.object = selected_;
    return apply(std::move(command));
}

/** Renames the selected object through the command path. */
bool EditorWorkspace::rename_selected(std::string name) {
    const project::scene::ForgeObject* const selected = selected_object();
    if (selected == nullptr || selected->editorLocked || name.empty()) {
        return false;
    }
    if (selected->editorName == name) {
        return true;
    }
    commands::Command command;
    command.kind = commands::CommandKind::renameObject;
    command.object = selected_;
    command.editorName = std::move(name);
    return apply(std::move(command));
}

/** Changes the selected object's logical kind through the command path. */
bool EditorWorkspace::set_selected_kind(core::ObjectKind kind) {
    const project::scene::ForgeObject* const selected = selected_object();
    if (selected == nullptr || selected->editorLocked) {
        return false;
    }
    if (selected->kind == kind) {
        return true;
    }
    commands::Command command;
    command.kind = commands::CommandKind::setObjectKind;
    command.object = selected_;
    command.objectKind = kind;
    return apply(std::move(command));
}

/** Changes the selected object's resource reference through the command path. */
bool EditorWorkspace::set_selected_resource(core::ResourceId resource) {
    const project::scene::ForgeObject* const selected = selected_object();
    if (selected == nullptr || selected->editorLocked) {
        return false;
    }
    if (selected->resource == resource) {
        return true;
    }
    commands::Command command;
    command.kind = commands::CommandKind::setResource;
    command.object = selected_;
    command.resource = resource;
    return apply(std::move(command));
}

/** Updates editor-only object flags through the command path. */
bool EditorWorkspace::set_selected_editor_flags(bool visible, bool locked) {
    const project::scene::ForgeObject* const selected = selected_object();
    if (selected == nullptr) {
        return false;
    }
    if (selected->editorVisible == visible && selected->editorLocked == locked) {
        return true;
    }
    commands::Command command;
    command.kind = commands::CommandKind::setEditorFlags;
    command.object = selected_;
    command.editorVisible = visible;
    command.editorLocked = locked;
    return apply(std::move(command));
}

/** Reparents one object through the command path. */
bool EditorWorkspace::reparent_object(core::ForgeUUID id, core::ForgeUUID parent) {
    const project::scene::ForgeObject* const object = scene_.find(id);
    if (object == nullptr || object->editorLocked) {
        return false;
    }
    if (object->parent == parent) {
        return true;
    }
    commands::Command command;
    command.kind = commands::CommandKind::reparentObject;
    command.object = id;
    command.parent = parent;
    return apply(std::move(command));
}

/** Selects one existing logical object. */
bool EditorWorkspace::select(core::ForgeUUID id) noexcept {
    if (!scene_.contains(id)) {
        return false;
    }
    selected_ = id;
    return true;
}

/** Applies a transform edit to the selected logical object. */
bool EditorWorkspace::set_selected_transform(core::Transform transform) {
    const project::scene::ForgeObject* const selected = selected_object();
    if (selected == nullptr || selected->editorLocked) {
        return false;
    }
    commands::Command command;
    command.kind = commands::CommandKind::setTransform;
    command.object = selected_;
    command.transform = transform;
    return apply(std::move(command));
}

/** Applies an uncommitted transform preview to the selected object. */
bool EditorWorkspace::preview_selected_transform(core::Transform transform) noexcept {
    const project::scene::ForgeObject* const selected = selected_object();
    if (selected == nullptr || selected->editorLocked) {
        return false;
    }
    return scene_.set_transform(selected_, transform);
}

/** Commits a previewed transform as one undoable transaction. */
bool EditorWorkspace::commit_selected_transform(core::Transform before, core::Transform after) {
    const project::scene::ForgeObject* const selected = selected_object();
    if (selected == nullptr || selected->editorLocked || !before.is_finite()
        || !after.is_finite()) {
        return false;
    }
    if (same_transform(before, after)) {
        return true;
    }
    if (!scene_.set_transform(selected_, after)) {
        return false;
    }

    commands::Command redo;
    redo.kind = commands::CommandKind::setTransform;
    redo.object = selected_;
    redo.transform = after;

    commands::Command undo;
    undo.kind = commands::CommandKind::setTransform;
    undo.object = selected_;
    undo.transform = before;

    commit_transaction(std::move(redo), std::move(undo), "Move object");
    return true;
}

/** Applies the latest undo transaction. */
bool EditorWorkspace::undo() {
    commands::Transaction transaction;
    if (!history_.undo(transaction)) {
        return false;
    }
    return apply_transaction(transaction.undoCommands);
}

/** Applies the latest redo transaction. */
bool EditorWorkspace::redo() {
    commands::Transaction transaction;
    if (!history_.redo(transaction)) {
        return false;
    }
    return apply_transaction(transaction.redoCommands);
}

core::ForgeUUID EditorWorkspace::selected_id() const noexcept {
    return selected_;
}

const project::scene::ForgeObject* EditorWorkspace::selected_object() const noexcept {
    return scene_.find(selected_);
}

const project::scene::ForgeObject* EditorWorkspace::find_object(core::ForgeUUID id) const noexcept {
    return scene_.find(id);
}

std::span<const project::scene::ForgeObject> EditorWorkspace::objects() const noexcept {
    return scene_.objects();
}

SessionState EditorWorkspace::session_state() const noexcept {
    return sessionState_;
}

const BaseplateTemplate& EditorWorkspace::active_template() const noexcept {
    return kTemplates[activeTemplate_ < kTemplates.size() ? activeTemplate_ : 0];
}

std::string_view EditorWorkspace::last_launch_message() const noexcept {
    return lastLaunchMessage_;
}

std::size_t EditorWorkspace::command_count() const noexcept {
    return queue_.size();
}

std::size_t EditorWorkspace::undo_count() const noexcept {
    return history_.undo_count();
}

std::size_t EditorWorkspace::redo_count() const noexcept {
    return history_.redo_count();
}

bool EditorWorkspace::can_undo() const noexcept {
    return history_.can_undo();
}

bool EditorWorkspace::can_redo() const noexcept {
    return history_.can_redo();
}

const ObjectRuntimeBinding* EditorWorkspace::runtime_binding(core::ForgeUUID id) const noexcept {
    for (const ObjectRuntimeBinding& binding : runtimeBindings_) {
        if (binding.object == id) {
            return &binding;
        }
    }
    return nullptr;
}

runtime::CapabilitySet EditorWorkspace::runtime_capabilities() const noexcept {
    return forge_runtime().capabilities();
}

runtime::WorldContext EditorWorkspace::runtime_world() const noexcept {
    return forge_runtime().world();
}

std::string_view EditorWorkspace::last_runtime_message() const noexcept {
    return lastRuntimeMessage_;
}

/** Applies one command to the Forge-owned scene and records a transaction. */
bool EditorWorkspace::apply(commands::Command command) {
    queue_.push(command);
    commands::Command pending;
    if (!queue_.try_pop(pending)) {
        return false;
    }

    commands::Command inverse;
    if (!make_inverse(pending, inverse)) {
        return false;
    }
    if (pending.kind == commands::CommandKind::deleteObject) {
        apply_to_runtime(pending);
    }
    if (!apply_to_scene(pending)) {
        return false;
    }
    if (pending.kind != commands::CommandKind::deleteObject) {
        apply_to_runtime(pending);
    }

    std::string label = label_for_command(pending.kind);
    commit_transaction(std::move(pending), std::move(inverse), std::move(label));
    return true;
}

/** Applies one command directly to the scene without touching history. */
bool EditorWorkspace::apply_to_scene(const commands::Command& pending) {
    switch (pending.kind) {
    case commands::CommandKind::createObject: {
        project::scene::ForgeObject object =
            pending.objectSnapshot.has_value()
                ? *pending.objectSnapshot
                : project::scene::make_object(
                      pending.object, pending.objectKind, pending.resource, pending.transform);
        if (!pending.objectSnapshot.has_value()) {
            object.parent = pending.parent;
            object.editorName = pending.editorName;
            object.editorVisible = pending.editorVisible;
            object.editorLocked = pending.editorLocked;
        }
        const core::ForgeUUID id = object.id;
        if (!scene_.create(std::move(object))) {
            return false;
        }
        selected_ = id;
        return true;
    }
    case commands::CommandKind::deleteObject: {
        const project::scene::ForgeObject* const object = scene_.find(pending.object);
        const core::ForgeUUID parent = object == nullptr ? core::ForgeUUID{} : object->parent;
        if (!scene_.erase(pending.object)) {
            return false;
        }
        if (selected_ == pending.object) {
            selected_ = scene_.contains(parent) ? parent : core::ForgeUUID{};
        }
        return true;
    }
    case commands::CommandKind::setTransform:
        return scene_.set_transform(pending.object, pending.transform);
    case commands::CommandKind::renameObject: {
        project::scene::ForgeObject* const object = scene_.find(pending.object);
        if (object == nullptr || pending.editorName.empty()) {
            return false;
        }
        object->editorName = pending.editorName;
        return true;
    }
    case commands::CommandKind::setObjectKind: {
        project::scene::ForgeObject* const object = scene_.find(pending.object);
        if (object == nullptr) {
            return false;
        }
        object->kind = pending.objectKind;
        if (object->kind == core::ObjectKind::folder) {
            object->resource = {};
        }
        return true;
    }
    case commands::CommandKind::setResource: {
        project::scene::ForgeObject* const object = scene_.find(pending.object);
        if (object == nullptr) {
            return false;
        }
        object->resource = pending.resource;
        return true;
    }
    case commands::CommandKind::setEditorFlags: {
        project::scene::ForgeObject* const object = scene_.find(pending.object);
        if (object == nullptr) {
            return false;
        }
        object->editorVisible = pending.editorVisible;
        object->editorLocked = pending.editorLocked;
        return true;
    }
    case commands::CommandKind::reparentObject:
        return scene_.reparent(pending.object, pending.parent);
    }

    return false;
}

/** Mirrors a committed scene command into the current live runtime adapter when possible. */
void EditorWorkspace::apply_to_runtime(const commands::Command& command) {
    switch (command.kind) {
    case commands::CommandKind::createObject:
    case commands::CommandKind::setObjectKind:
    case commands::CommandKind::setResource: {
        if (command.kind != commands::CommandKind::createObject) {
            destroy_runtime_object(command.object, "rebind");
        }
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object != nullptr) {
            bind_runtime_object(*object, label_for_command(command.kind));
        }
        return;
    }
    case commands::CommandKind::deleteObject:
        destroy_runtime_object(command.object, "delete");
        return;
    case commands::CommandKind::setTransform: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object != nullptr) {
            transform_runtime_object(*object, "Set transform");
        }
        return;
    }
    case commands::CommandKind::renameObject:
    case commands::CommandKind::setEditorFlags:
    case commands::CommandKind::reparentObject:
        return;
    }
}

/** Attempts to create or mark the runtime binding for one authored object. */
void EditorWorkspace::bind_runtime_object(const project::scene::ForgeObject& object,
                                          std::string_view reason) {
    runtime::SpawnResult spawn{};
    runtime::RuntimeStatus status = runtime::RuntimeStatus::unsupported;
    runtime::ForgeHandle handle{};
    std::string_view action = "local";
    std::string_view detail = reason;

    switch (object.kind) {
    case core::ObjectKind::forgeOnly:
    case core::ObjectKind::folder:
        status = runtime::RuntimeStatus::ok;
        detail = "editor_only";
        break;
    case core::ObjectKind::staticInstance:
        action = "spawn_static";
        if (!object.resource.is_valid()) {
            status = runtime::RuntimeStatus::invalidArgument;
            detail = "missing_resource";
            break;
        }
        spawn = forge_runtime().spawn_static(object.resource, object.transform);
        status = spawn.status;
        handle = spawn.handle;
        break;
    case core::ObjectKind::patternInstance:
        action = "spawn_pattern";
        if (!object.resource.is_valid()) {
            status = runtime::RuntimeStatus::invalidArgument;
            detail = "missing_resource";
            break;
        }
        spawn = forge_runtime().spawn_pattern(object.resource, object.transform);
        status = spawn.status;
        handle = spawn.handle;
        break;
    case core::ObjectKind::entityInstance:
        action = "spawn_entity";
        detail = "entity_spawn_not_bound";
        break;
    }

    record_runtime(object.id, action, status, handle, detail);
}

/** Destroys a live runtime binding for one object when a native handle exists. */
void EditorWorkspace::destroy_runtime_object(core::ForgeUUID id, std::string_view reason) {
    const ObjectRuntimeBinding* const binding = runtime_binding(id);
    if (binding == nullptr) {
        return;
    }

    runtime::RuntimeStatus status = runtime::RuntimeStatus::unavailable;
    if (binding->handle.is_valid()) {
        status = forge_runtime().destroy(binding->handle);
    }
    record_runtime(id, "destroy", status, {}, reason);
    remove_runtime_binding(id);
}

/** Writes one authored transform to the live runtime when the object has a native handle. */
void EditorWorkspace::transform_runtime_object(const project::scene::ForgeObject& object,
                                               std::string_view reason) {
    const ObjectRuntimeBinding* const binding = runtime_binding(object.id);
    if (binding == nullptr || !binding->handle.is_valid()) {
        record_runtime(
            object.id, "set_transform", runtime::RuntimeStatus::unavailable, {}, "unbound");
        return;
    }

    const runtime::RuntimeStatus status =
        forge_runtime().set_transform(binding->handle, object.transform);
    record_runtime(object.id, "set_transform", status, binding->handle, reason);
}

/** Returns the runtime binding record for one object, creating a blank record when absent. */
ObjectRuntimeBinding& EditorWorkspace::runtime_binding_for(core::ForgeUUID id) {
    for (ObjectRuntimeBinding& binding : runtimeBindings_) {
        if (binding.object == id) {
            return binding;
        }
    }
    runtimeBindings_.push_back({.object = id});
    return runtimeBindings_.back();
}

/** Removes any volatile native runtime binding for one object. */
void EditorWorkspace::remove_runtime_binding(core::ForgeUUID id) {
    runtimeBindings_.erase(
        std::remove_if(runtimeBindings_.begin(),
                       runtimeBindings_.end(),
                       [id](const ObjectRuntimeBinding& binding) { return binding.object == id; }),
        runtimeBindings_.end());
}

/** Records one runtime bridge attempt in memory and in the structured log. */
void EditorWorkspace::record_runtime(core::ForgeUUID id,
                                     std::string_view action,
                                     runtime::RuntimeStatus status,
                                     runtime::ForgeHandle handle,
                                     std::string_view detail) {
    ObjectRuntimeBinding& binding = runtime_binding_for(id);
    binding.handle = handle;
    binding.lastStatus = status;
    binding.lastAction.assign(action.data(), action.size());
    binding.lastDetail.assign(detail.data(), detail.size());

    const std::array<char, 17> idText = uuid_suffix(id);
    const std::string_view statusText = runtime::status_text(status);
    std::array<char, 320> message{};
    const int messageWritten = std::snprintf(message.data(),
                                             message.size(),
                                             "%.*s: %.*s (%.*s)",
                                             static_cast<int>(action.size()),
                                             action.data(),
                                             static_cast<int>(detail.size()),
                                             detail.data(),
                                             static_cast<int>(statusText.size()),
                                             statusText.data());
    lastRuntimeMessage_ =
        messageWritten > 0 ? std::string(message.data(), static_cast<std::size_t>(messageWritten))
                           : std::string{"Runtime bridge updated."};

    std::array<char, ::sunrise::core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=izanami_runtime action=%.*s result=%.*s object=%s handle=0x%llX "
                      "detail=%.*s",
                      static_cast<int>(action.size()),
                      action.data(),
                      static_cast<int>(statusText.size()),
                      statusText.data(),
                      idText.data(),
                      static_cast<unsigned long long>(handle.value),
                      static_cast<int>(detail.size()),
                      detail.data());
    if (written > 0) {
        ::sunrise::core::log::write(::sunrise::core::log::Channel::client,
                                    status == runtime::RuntimeStatus::ok
                                        ? ::sunrise::core::log::Level::info
                                        : ::sunrise::core::log::Level::warn,
                                    {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Builds the inverse of one command from the current scene state before the edit. */
bool EditorWorkspace::make_inverse(const commands::Command& command,
                                   commands::Command& inverse) const {
    switch (command.kind) {
    case commands::CommandKind::createObject:
        inverse.kind = commands::CommandKind::deleteObject;
        inverse.object = command.object;
        return true;
    case commands::CommandKind::deleteObject: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object == nullptr) {
            return false;
        }
        inverse.kind = commands::CommandKind::createObject;
        inverse.object = object->id;
        inverse.objectSnapshot = *object;
        return true;
    }
    case commands::CommandKind::setTransform: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object == nullptr) {
            return false;
        }
        inverse.kind = commands::CommandKind::setTransform;
        inverse.object = command.object;
        inverse.transform = object->transform;
        return true;
    }
    case commands::CommandKind::renameObject: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object == nullptr) {
            return false;
        }
        inverse.kind = commands::CommandKind::renameObject;
        inverse.object = command.object;
        inverse.editorName = object->editorName;
        return true;
    }
    case commands::CommandKind::setObjectKind: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object == nullptr) {
            return false;
        }
        inverse.kind = commands::CommandKind::setObjectKind;
        inverse.object = command.object;
        inverse.objectKind = object->kind;
        return true;
    }
    case commands::CommandKind::setResource: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object == nullptr) {
            return false;
        }
        inverse.kind = commands::CommandKind::setResource;
        inverse.object = command.object;
        inverse.resource = object->resource;
        return true;
    }
    case commands::CommandKind::setEditorFlags: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object == nullptr) {
            return false;
        }
        inverse.kind = commands::CommandKind::setEditorFlags;
        inverse.object = command.object;
        inverse.editorVisible = object->editorVisible;
        inverse.editorLocked = object->editorLocked;
        return true;
    }
    case commands::CommandKind::reparentObject: {
        const project::scene::ForgeObject* const object = scene_.find(command.object);
        if (object == nullptr) {
            return false;
        }
        inverse.kind = commands::CommandKind::reparentObject;
        inverse.object = command.object;
        inverse.parent = object->parent;
        return true;
    }
    }
    return false;
}

/** Applies a stored transaction side without recording a new transaction. */
bool EditorWorkspace::apply_transaction(const std::vector<commands::Command>& commands) {
    for (const commands::Command& command : commands) {
        if (command.kind == commands::CommandKind::deleteObject) {
            apply_to_runtime(command);
        }
        if (!apply_to_scene(command)) {
            return false;
        }
        if (command.kind != commands::CommandKind::deleteObject) {
            apply_to_runtime(command);
        }
    }
    return true;
}

/** Stores one completed editor edit in the transaction history. */
void EditorWorkspace::commit_transaction(commands::Command redo,
                                         commands::Command undo,
                                         std::string label) {
    commands::Transaction transaction;
    transaction.redoCommands.push_back(std::move(redo));
    transaction.undoCommands.push_back(std::move(undo));
    transaction.author = "local";
    transaction.label = std::move(label);
    report("commit", "ok", transaction.label);
    history_.commit(std::move(transaction));
}

core::ForgeUUID EditorWorkspace::next_uuid() noexcept {
    return core::make_forge_uuid(kIzanamiUuidDomain, nextId_++);
}

/** @return Process-local editor workspace state. */
EditorWorkspace& workspace() noexcept {
    return g_workspace;
}

} // namespace sunrise::izanami::editor::workspace
