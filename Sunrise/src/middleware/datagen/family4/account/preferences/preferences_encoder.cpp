#include "preferences_encoder.h"

#include "../../../../../state/account/settings/native_key_binding_map.h"

namespace sunrise::middleware::datagen::family4::account::preferences {
namespace {

namespace bindings = state::account::settings::bindings;

/** Native keybinding halves use input code 0x74 as the unbound sentinel. */
constexpr std::uint16_t kUnboundInputCode = 0x0074;
/** Seed version 0 lets the client set up local mirrors once after sign-in. */
constexpr std::int32_t kOpenSeedVersion = 0;
/**
 * Seed version 1 closes a gate so the client keeps the replicated values behind it.
 * The keybinding gate stays closed because Sunrise supplies the modeled preferences. The
 * post-processing gate stays closed, or local cvars overwrite its 3 replicated fields each login.
 */
constexpr std::int32_t kClosedSeedVersion = 1;
/** Source 0 makes later input reads use the replicated keybinding array. */
constexpr std::uint8_t kReplicatedBindingSource = 0;
/** Source 1 makes later input reads use the computer-local keybindings. */
constexpr std::uint8_t kComputerBindingSource = 1;

/**
 * Converts a semantic boolean to the native 1-byte form.
 * @return 1 for true, 0 for false.
 */
[[nodiscard]] constexpr std::uint8_t native_boolean(bool value) noexcept {
    return value ? 1U : 0U;
}

/**
 * Packs semantic primary and secondary codes into one native replicated value.
 * @param binding Authored optional input halves.
 * @return Secondary input in the high word and primary input in the low word.
 */
[[nodiscard]] constexpr std::uint32_t
pack_binding(const state::account::settings::bindings::Binding& binding) noexcept {
    const std::uint32_t primary = binding.primary.value_or(kUnboundInputCode);
    const std::uint32_t secondary = binding.secondary.value_or(kUnboundInputCode);
    /** A 16-bit word separates the native primary and secondary input halves. */
    constexpr unsigned kSecondaryWordShift = 16;
    return (secondary << kSecondaryWordShift) | primary;
}

} // namespace

/** Maps semantic State preferences into the two native account records. */
bool encode(const state::account::settings::AccountSettings& settings,
            Record& record,
            BindingsRecord& bindingsRecord) noexcept {
    if (!state::account::settings::valid(settings)) {
        return false;
    }

    record = {};
    bindingsRecord = {};
    record.postProcessingSeedVersion = kClosedSeedVersion;
    bindingsRecord.accountSeedVersion = kClosedSeedVersion;
    bindingsRecord.sourceSelector =
        settings.keyBindingSource == state::account::settings::KeyBindingSource::account
            ? kReplicatedBindingSource
            : kComputerBindingSource;

    const auto& controls = settings.controls;
    record.buttonLayout = controls.buttonLayout;
    record.movementMode = controls.movementMode;
    record.controllerLookSensitivity = controls.controllerLookSensitivity;
    record.doublePressDelay = controls.doublePressDelay;
    record.mouseLookSensitivity = controls.mouseLookSensitivity;
    record.adsSensitivityModifier = controls.adsSensitivityModifier;
    record.controllerInvertVertical = native_boolean(controls.controllerInvertVertical);
    record.controllerInvertHorizontal = native_boolean(controls.controllerInvertHorizontal);
    record.mouseInvertVertical = native_boolean(controls.mouseInvertVertical);
    record.mouseInvertHorizontal = native_boolean(controls.mouseInvertHorizontal);
    record.controllerAutoLookCentering = native_boolean(controls.controllerAutoLookCentering);
    record.controllerVibration = native_boolean(controls.controllerVibration);
    record.unidentifiedToggle = native_boolean(controls.unidentifiedToggle);
    record.mouseAimSmoothing = native_boolean(controls.mouseAimSmoothing);
    record.controllerSwapShoulders = native_boolean(controls.controllerSwapShoulders);

    const auto& audio = settings.audio;
    record.voiceOutputMode = audio.voiceOutputMode;
    record.teamVoiceChannel = audio.teamVoiceChannel;
    record.reservedAudioMode = audio.reservedMode;
    record.audioMigrationVersion = audio.migrationVersion;
    record.chatVolume = audio.chatVolume;
    record.muteWhenUnfocused = native_boolean(audio.muteWhenUnfocused);
    record.soundEffectsVolume = audio.soundEffectsVolume;
    record.dialogueVolume = audio.dialogueVolume;
    record.musicVolume = audio.musicVolume;

    const auto& display = settings.display;
    record.brightness = display.brightness;
    record.showFps = native_boolean(display.showFps);
    record.hdrMode = display.hdrMode;
    bindingsRecord.verticalSyncMirror = display.verticalSyncInterval;
    bindingsRecord.fieldOfViewAdjustment = display.fieldOfView;
    record.calibrationPrimary = display.calibrationPrimary;
    record.calibrationAlpha = display.calibrationAlpha;

    const auto& interfaceSettings = settings.interface;
    record.subtitlesMode = interfaceSettings.subtitlesMode;
    record.colorblindMode = interfaceSettings.colorblindMode;
    record.helmetMode = interfaceSettings.helmetMode;
    record.hudOpacity = interfaceSettings.hudOpacity;
    record.displayHints = native_boolean(interfaceSettings.displayHints);
    record.backgroundOpacity = interfaceSettings.backgroundOpacity;
    record.reticleLocation = interfaceSettings.reticleLocation;
    record.reticleColor = interfaceSettings.reticleColor;
    record.textSize = interfaceSettings.textSize;
    record.textColor = interfaceSettings.textColor;
    record.textBackgroundStyle = interfaceSettings.textBackgroundStyle;
    record.textBackgroundOpacity = interfaceSettings.textBackgroundOpacity;
    record.reservedTextMode = interfaceSettings.reservedTextMode;
    record.subtitleOptionsEntry = interfaceSettings.subtitleOptionsEntry;

    const auto& social = settings.social;
    record.preferGoodConnection = native_boolean(social.preferGoodConnection);
    record.textChatMode = social.textChatMode;
    record.showRealNames = native_boolean(social.showRealNames);
    record.clanInviteNotifications = native_boolean(social.clanInviteNotifications);
    record.profanityFilter = native_boolean(social.profanityFilter);
    record.voiceChatEnabled = native_boolean(social.voiceChatEnabled);
    record.whisperChatMode = social.whisperChatMode;
    record.teamChatJoinMode = social.teamChatJoinMode;
    record.localChatJoinMode = social.localChatJoinMode;
    record.clanChatJoinMode = social.clanChatJoinMode;
    record.chatAutoHideMode = social.chatAutoHideMode;
    bindingsRecord.voiceChatMirror = native_boolean(social.voiceChatEnabled);

    for (std::size_t nativeSlot = 0; nativeSlot < bindings::kActionsByNativeSlot.size();
         ++nativeSlot) {
        const auto action = bindings::kActionsByNativeSlot[nativeSlot];
        const std::size_t stateIndex = static_cast<std::size_t>(action);
        // Native ABI order stays independent from the semantic State enum order.
        bindingsRecord.keyBindings[nativeSlot] =
            pack_binding(settings.keyBindings.values[stateIndex]);
    }
    return true;
}

} // namespace sunrise::middleware::datagen::family4::account::preferences
