#include "director_handoff.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/account/account_state.h"
#include "../../../state/activity/defaults/activity_defaults_snapshot.h"
#include "../../../state/activity/destination/definition.h"
#include "../../../state/runtime/runtime.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../bootflow/bootflow_hook_lifecycle.h"
#include "../polled_input/runtime.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::director {
namespace {

namespace bindings = state::account::settings::bindings;

/** Frames the synthetic key remains visible to the game's own key scan. */
constexpr std::uint32_t kPulseFrames = 4;
/** Default authored binding for `ui_open_director` in resources/default_settings.json. */
constexpr std::uint32_t kFallbackDirectorKey = 'M';
/** Native boot-flow step `setup:activity_session_creation`. */
constexpr std::int32_t kActivitySessionCreation = 30;
/** Native world-controller goal reached by a successful Director activity launch. */
constexpr std::int32_t kInWorldGoal = 38;
/** Goal rows use mode one in orbit and mode two for an activity transition. */
constexpr std::uint8_t kActivityGoalMode = 2;

/**
 * Wrapper that asks the boot-flow manager for cleanup state 0x1c. The caller's argument is the
 * reason, not the destination state. Its manager call and generic request-state jump expose the
 * two native targets Izanami needs; only their displacements are wildcarded.
 */
constexpr std::string_view kRequestBootflowStepText =
    "40 53 48 83 EC 20 8B D9 E8 ? ? ? ? 48 85 C0 74 15 44 8B C3 BA 1C 00 00 00 48 8B C8 "
    "48 83 C4 20 5B E9 ? ? ? ?";
constexpr auto kRequestBootflowStep =
    patterns::signature<patterns::signature_length(kRequestBootflowStepText)>(
        kRequestBootflowStepText);

/**
 * Resolves one of the six live networking sessions. Index zero is the active private session in
 * orbit. The wildcarded call obtains the obfuscated session container.
 */
constexpr std::string_view kGetLiveSessionText =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 63 F9 E8 ? ? ? ? 48 85 C0 74 4D 83 FF FF "
    "74 48 45 32 C9 44 38 48 08 74 30 4C 69 C7 48 91 03 00 4C 03 C0 49 63 40 10";
constexpr auto kGetLiveSession =
    patterns::signature<patterns::signature_length(kGetLiveSessionText)>(kGetLiveSessionText);

/**
 * Publishes `world-controller-goal-data` through the native session-parameter object. Besides
 * Changing the target and mode advances the parameter revision and invokes its replication
 * callback. Writing the membership's replicated copy directly leaves its checksum stale.
 */
constexpr std::string_view kPublishSessionGoalText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F8 8B F2 48 8B D9 E8 ? ? ? ? "
    "48 8B CB 84 C0 74 ? E8 ? ? ? ? 84 C0 74 ? 3B B3 48 01 00 00 75 ? 3B BB 4C 01 00 00 "
    "74 ? 89 B3 48 01 00 00 B9 01 00 00 00 89 BB 4C 01 00 00";
constexpr auto kPublishSessionGoal =
    patterns::signature<patterns::signature_length(kPublishSessionGoalText)>(
        kPublishSessionGoalText);

/**
 * World-controller setup asks this zero-argument getter for its secondary activity-selection
 * index, then builds and launches a local selection from it. A direct state-30 jump otherwise
 * receives index zero, whose placeholder selection never creates a world container.
 */
constexpr std::string_view kSecondarySelectionCallText =
    "48 8D 4C 24 28 E8 ? ? ? ? 0F B7 4C 24 20 66 83 F9 FF 74 5A E8 ? ? ? ? 84 C0 "
    "75 51 48 8D 4C 24 30";
constexpr auto kSecondarySelectionCall =
    patterns::signature<patterns::signature_length(kSecondarySelectionCallText)>(
        kSecondarySelectionCallText);

using GetBootflowManager = void*(__fastcall*)() noexcept;
using RequestBootflowState = void(__fastcall*)(void*, std::int32_t, std::int32_t) noexcept;
using GetLiveSession = bool(__fastcall*)(std::int32_t, void**) noexcept;
using PublishSessionGoal = bool(__fastcall*)(void*, std::int32_t, std::int32_t) noexcept;
using GetSecondarySelection = std::uint16_t*(__fastcall*)(std::uint16_t*) noexcept;

/** Relative operands inside the matched wrapper's manager call and generic state-request jump. */
constexpr std::size_t kManagerAccessorOperand = 9;
constexpr std::size_t kManagerAccessorEnd = 13;
constexpr std::size_t kStateRequestOperand = 35;
constexpr std::size_t kStateRequestEnd = 39;
/** Relative call operand and return address inside kSecondarySelectionCall. */
constexpr std::size_t kSecondarySelectionOperand = 6;
constexpr std::size_t kSecondarySelectionReturn = 10;
/** The native log names zero as the default/unavailable state-change reason. */
constexpr std::int32_t kDefaultStateChangeReason = 0;

/** Native session-parameter collection; its first object is `world-controller-goal-data`. */
constexpr std::size_t kSessionParametersOffset = 0xF4B8;
constexpr std::int32_t kActivePrivateSession = 0;

std::atomic_uint32_t g_virtualKey{};
std::atomic_uint32_t g_frames{};
std::atomic_bool g_actionKeysResolved{false};
std::atomic<GetBootflowManager> g_getBootflowManager{nullptr};
std::atomic<RequestBootflowState> g_requestBootflowState{nullptr};
std::atomic<GetLiveSession> g_getLiveSession{nullptr};
std::atomic<PublishSessionGoal> g_publishSessionGoal{nullptr};
std::atomic_bool g_activityLaunchPending{false};
std::atomic_bool g_carrierOverrideArmed{false};
std::atomic<std::int16_t> g_carrierTarget{-1};
std::atomic<void*> g_secondarySelectionReturn{nullptr};
hooking::detour::Handle g_secondarySelectionHandle{};
std::atomic_bool g_secondarySelectionInstalled{false};

/** Logs installation of Forge's one-shot native carrier. */
void report_carrier_install(std::string_view result, std::int16_t targetIndex) noexcept {
    std::array<char, 160> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=director_activity_launch stage=local_carrier result=%.*s "
                      "fallback=%d armed=%u",
                      static_cast<int>(result.size()),
                      result.data(),
                      static_cast<int>(targetIndex),
                      g_carrierOverrideArmed.load(std::memory_order_acquire) ? 1U : 0U);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == "installed" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Logs an exact-caller observation without consulting State from inside the detour. */
void report_carrier_observation(std::string_view action,
                                std::uint16_t nativeIndex,
                                std::int16_t targetIndex) noexcept {
    std::array<char, 176> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=director_activity_launch stage=local_carrier result=observed "
                      "native=%u fallback=%d action=%.*s",
                      static_cast<unsigned>(nativeIndex),
                      static_cast<int>(targetIndex),
                      static_cast<int>(action.size()),
                      action.data());
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Substitutes the configured, validated fallback activity only for the setup caller resolved
 * above. The server still owns the forced package rewrite; this supplies the client-side activity
 * identity that its world loader requires before it can bind an ActivityClient.
 */
std::uint16_t* __fastcall get_secondary_selection(std::uint16_t* output) noexcept {
    const auto original =
        reinterpret_cast<GetSecondarySelection>(g_secondarySelectionHandle.original);
    std::uint16_t* const result = original != nullptr ? original(output) : output;
    if (result == nullptr || output == nullptr
        || _ReturnAddress() != g_secondarySelectionReturn.load(std::memory_order_acquire)) {
        return result;
    }

    const std::uint16_t native = *result;
    const std::int16_t target = g_carrierTarget.load(std::memory_order_acquire);
    if (native != 0) {
        report_carrier_observation("preserve_nonzero", native, target);
        return result;
    }
    if (target < 0 || !g_carrierOverrideArmed.exchange(false, std::memory_order_acq_rel)) {
        return result;
    }
    *result = static_cast<std::uint16_t>(target);
    report_carrier_observation("substitute", native, target);
    return result;
}

/** Resolves the manager accessor and generic state request from the matched cleanup wrapper. */
[[nodiscard]] bool resolve_activity_launch_target() noexcept {
    if (g_getBootflowManager.load(std::memory_order_acquire) != nullptr
        && g_requestBootflowState.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    std::byte* const wrapper =
        patterns::scan_main_image_unique(kRequestBootflowStep, "director_activity_launch");
    if (wrapper == nullptr) {
        return false;
    }
    const auto getManager = reinterpret_cast<GetBootflowManager>(patterns::resolve_relative(
        wrapper + kManagerAccessorOperand, wrapper + kManagerAccessorEnd));
    const auto requestState = reinterpret_cast<RequestBootflowState>(
        patterns::resolve_relative(wrapper + kStateRequestOperand, wrapper + kStateRequestEnd));
    if (getManager == nullptr || requestState == nullptr) {
        return false;
    }
    g_getBootflowManager.store(getManager, std::memory_order_release);
    g_requestBootflowState.store(requestState, std::memory_order_release);
    return true;
}

/** Resolves the native live-session lookup and authoritative goal publisher. */
[[nodiscard]] bool resolve_session_goal_targets() noexcept {
    if (g_getLiveSession.load(std::memory_order_acquire) != nullptr
        && g_publishSessionGoal.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    std::byte* const sessionTarget =
        patterns::scan_main_image_unique(kGetLiveSession, "director_session_goal");
    std::byte* const publishTarget =
        patterns::scan_main_image_unique(kPublishSessionGoal, "director_session_goal_publish");
    if (sessionTarget == nullptr || publishTarget == nullptr) {
        return false;
    }
    g_getLiveSession.store(reinterpret_cast<GetLiveSession>(sessionTarget),
                           std::memory_order_release);
    g_publishSessionGoal.store(reinterpret_cast<PublishSessionGoal>(publishTarget),
                               std::memory_order_release);
    return true;
}

/** Resolves and detours the secondary local-selection getter used by setup state 30. */
[[nodiscard]] bool resolve_secondary_selection_target() noexcept {
    if (g_secondarySelectionInstalled.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const call =
        patterns::scan_main_image_unique(kSecondarySelectionCall, "director_local_carrier");
    if (call == nullptr) {
        return false;
    }
    void* const target = patterns::resolve_relative(call + kSecondarySelectionOperand,
                                                    call + kSecondarySelectionReturn);
    if (target == nullptr) {
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&get_secondary_selection)};
    if (!hooking::detour::install(spec, g_secondarySelectionHandle)) {
        return false;
    }
    g_secondarySelectionReturn.store(call + kSecondarySelectionReturn, std::memory_order_release);
    g_secondarySelectionInstalled.store(true, std::memory_order_release);
    return true;
}

/** Logs one stage of the native activity launch. */
void report_activity_launch(std::string_view stage, std::string_view result) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=director_activity_launch stage=%.*s result=%.*s",
                                      static_cast<int>(stage.size()),
                                      stage.data(),
                                      static_cast<int>(result.size()),
                                      result.data());
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == "ok" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Logs publication of Izanami's goal through Destiny's session-parameter system. */
void report_goal_publish(std::string_view result) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=director_activity_launch stage=goal_publish result=%.*s "
                                      "target=%d mode=%u",
                                      static_cast<int>(result.size()),
                                      result.data(),
                                      kInWorldGoal,
                                      static_cast<unsigned>(kActivityGoalMode));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == "ok" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Mirrors the native Director launch at the authoritative parameter boundary. The publisher
 * advances the goal revision and queues replication, so Destiny computes matching peer-property
 * checksums before activity-session creation consumes the goal.
 */
[[nodiscard]] bool publish_activity_goal() noexcept {
    const GetLiveSession getSession = g_getLiveSession.load(std::memory_order_acquire);
    const PublishSessionGoal publishGoal = g_publishSessionGoal.load(std::memory_order_acquire);
    void* session = nullptr;
    if (getSession == nullptr || publishGoal == nullptr
        || !getSession(kActivePrivateSession, &session) || session == nullptr) {
        report_goal_publish("session_missing");
        return false;
    }

    void* const parameters = static_cast<std::byte*>(session) + kSessionParametersOffset;
    const bool published = publishGoal(parameters, kInWorldGoal, kActivityGoalMode);
    report_goal_publish(published ? "ok" : "rejected");
    return published;
}

/** @return True once the game's input-code tables have been found. */
[[nodiscard]] bool ensure_action_keys() noexcept {
    if (g_actionKeysResolved.load(std::memory_order_acquire)) {
        return true;
    }
    const bool resolved = teleport::resolve_action_keys();
    if (resolved) {
        g_actionKeysResolved.store(true, std::memory_order_release);
    }
    return resolved;
}

/**
 * Converts one authored binding half to a Windows virtual-key.
 * @param binding Input binding half from replicated account settings.
 * @param key Receives the virtual key.
 * @return True when the half names a keyboard key.
 */
[[nodiscard]] bool binding_key(std::uint16_t binding, std::uint32_t& key) noexcept {
    if (!ensure_action_keys()) {
        return false;
    }
    key = teleport::action_key(binding);
    return key != 0;
}

/**
 * Converts one semantic action to its primary or secondary key.
 * @param account Current account snapshot.
 * @param action Semantic action to drive.
 * @param key Receives the virtual key.
 * @return True when the action is bound to a keyboard key.
 */
[[nodiscard]] bool action_key(const state::AccountState& account,
                              bindings::Action action,
                              std::uint32_t& key) noexcept {
    const std::size_t index = static_cast<std::size_t>(action);
    if (index >= account.settings.keyBindings.values.size()) {
        return false;
    }
    const bindings::Binding& binding = account.settings.keyBindings.values[index];
    if (binding.primary.has_value() && binding_key(*binding.primary, key)) {
        return true;
    }
    return binding.secondary.has_value() && binding_key(*binding.secondary, key);
}

/** Logs the key path chosen for the native handoff. */
void report_request(const HandoffResult& result, std::uint32_t virtualKey) noexcept {
    std::array<char, 160> line{};
    const char* source = result.usedDestinationsTab
                             ? "destinations_tab"
                             : (result.usedDirector ? "director" : "fallback");
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=director_handoff stage=request result=%s source=%s "
                                      "vk=0x%X bindings=%u",
                                      result.requested ? "ok" : "fail",
                                      source,
                                      static_cast<unsigned>(virtualKey),
                                      result.accountBindingsConfigured ? 1U : 0U);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result.requested ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Logs that the synthetic key was released. */
void report_release(std::uint32_t virtualKey) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=director_handoff stage=pulse result=released vk=0x%X",
                                      static_cast<unsigned>(virtualKey));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Installs and arms the exact-caller local carrier before orbit constructs its selection. */
bool install_local_carrier() noexcept {
    if (g_secondarySelectionInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    const std::int16_t target = defaults.defaultDestination.selection.activityIndex;
    g_carrierTarget.store(target, std::memory_order_release);
    if (target < 0 || target > state::activity::destination::kMaximumActivityIndex) {
        report_carrier_install("invalid_fallback", target);
        return false;
    }
    if (!resolve_secondary_selection_target()) {
        g_carrierTarget.store(-1, std::memory_order_release);
        report_carrier_install("attach_failed", target);
        return false;
    }
    g_carrierOverrideArmed.store(true, std::memory_order_release);
    report_carrier_install("installed", target);
    return true;
}

/** Queues Destiny's own orbit-to-activity transition. */
ActivityLaunchResult request_activity_launch() noexcept {
    ActivityLaunchResult result{};
    result.targetResolved = resolve_activity_launch_target() && resolve_session_goal_targets()
                            && g_secondarySelectionInstalled.load(std::memory_order_acquire);
    result.inOrbit = bootflow::in_orbit();
    result.requested = result.targetResolved && result.inOrbit;
    if (result.requested) {
        g_activityLaunchPending.store(true, std::memory_order_release);
    }
    report_activity_launch(
        "request",
        result.requested ? "ok" : (result.targetResolved ? "not_in_orbit" : "target_missing"));
    return result;
}

/** Requests a short native key pulse that opens Destiny's Director. */
HandoffResult request_open_destinations() noexcept {
    const state::AccountState account = state::account_snapshot();
    HandoffResult result{};
    result.accountBindingsConfigured = account.settings.keyBindings.configured;

    std::uint32_t virtualKey = 0;
    if (action_key(account, bindings::Action::uiOpenDirectorDestinationsTab, virtualKey)) {
        result.usedDestinationsTab = true;
    } else if (action_key(account, bindings::Action::uiOpenDirector, virtualKey)) {
        result.usedDirector = true;
    } else {
        virtualKey = kFallbackDirectorKey;
        result.usedFallback = true;
    }

    result.requested = virtualKey != 0;
    if (result.requested) {
        g_virtualKey.store(virtualKey, std::memory_order_release);
        g_frames.store(kPulseFrames, std::memory_order_release);
    }
    report_request(result, virtualKey);
    return result;
}

/** Emits and releases any pending Director key pulse. */
void poll() noexcept {
    if (g_activityLaunchPending.exchange(false, std::memory_order_acq_rel)) {
        const GetBootflowManager getManager = g_getBootflowManager.load(std::memory_order_acquire);
        const RequestBootflowState requestState =
            g_requestBootflowState.load(std::memory_order_acquire);
        if (getManager == nullptr || requestState == nullptr) {
            report_activity_launch("invoke", "target_missing");
        } else if (!bootflow::in_orbit()) {
            report_activity_launch("invoke", "left_orbit");
        } else if (!publish_activity_goal()) {
            report_activity_launch("invoke", "goal_publish_failed");
        } else if (void* const manager = getManager(); manager != nullptr) {
            requestState(manager, kActivitySessionCreation, kDefaultStateChangeReason);
            report_activity_launch("invoke", "ok");
        } else {
            report_activity_launch("invoke", "manager_missing");
        }
    }

    const std::uint32_t frames = g_frames.load(std::memory_order_acquire);
    if (frames == 0) {
        return;
    }
    const std::uint32_t virtualKey = g_virtualKey.load(std::memory_order_acquire);
    if (virtualKey == 0) {
        g_frames.store(0, std::memory_order_release);
        return;
    }
    polled_input::hold_key(virtualKey);
    if (g_frames.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
        polled_input::release_key();
        g_virtualKey.store(0, std::memory_order_release);
        report_release(virtualKey);
    }
}

/** Cancels a pending Director key pulse and releases the spoofed key. */
void cancel() noexcept {
    g_activityLaunchPending.store(false, std::memory_order_release);
    g_frames.store(0, std::memory_order_release);
    g_virtualKey.store(0, std::memory_order_release);
    g_actionKeysResolved.store(false, std::memory_order_release);
    polled_input::release_key();
}

/** Cancels pending work and detaches the local-selection hook before client teardown. */
void shutdown() noexcept {
    cancel();
    g_carrierOverrideArmed.store(false, std::memory_order_release);
    g_carrierTarget.store(-1, std::memory_order_release);
    if (!g_secondarySelectionInstalled.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)hooking::detour::uninstall(g_secondarySelectionHandle);
    g_secondarySelectionHandle = {};
    g_secondarySelectionReturn.store(nullptr, std::memory_order_release);
}

} // namespace sunrise::client::hooks::director
