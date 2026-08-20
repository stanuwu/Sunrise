#include "settings_delta.h"

#include <optional>

#include "settings_state.h"

namespace sunrise::state::account::settings {

/** Assigns a sparse value without treating present zero or false as absence. */
template <typename Value>
static void apply_if_present(const std::optional<Value>& update, Value& target) noexcept {
    if (update.has_value()) {
        target = *update;
    }
}

/** Applies the supported controller and mouse fields. */
static void apply_controls(const ControlsDelta& update, Controls& target) noexcept {
    apply_if_present(update.buttonLayout, target.buttonLayout);
    apply_if_present(update.movementMode, target.movementMode);
    apply_if_present(update.controllerLookSensitivity, target.controllerLookSensitivity);
    apply_if_present(update.controllerInvertVertical, target.controllerInvertVertical);
    apply_if_present(update.controllerAutoLookCentering, target.controllerAutoLookCentering);
    apply_if_present(update.controllerVibration, target.controllerVibration);
    apply_if_present(update.controllerSwapShoulders, target.controllerSwapShoulders);
    apply_if_present(update.controllerInvertHorizontal, target.controllerInvertHorizontal);
    apply_if_present(update.mouseLookSensitivity, target.mouseLookSensitivity);
    apply_if_present(update.mouseInvertVertical, target.mouseInvertVertical);
    apply_if_present(update.mouseInvertHorizontal, target.mouseInvertHorizontal);
    apply_if_present(update.unidentifiedToggle, target.unidentifiedToggle);
    apply_if_present(update.mouseAimSmoothing, target.mouseAimSmoothing);
    apply_if_present(update.adsSensitivityModifier, target.adsSensitivityModifier);
    apply_if_present(update.doublePressDelay, target.doublePressDelay);
}

/** Applies the supported voice and volume fields. */
static void apply_audio(const AudioDelta& update, Audio& target) noexcept {
    apply_if_present(update.voiceOutputMode, target.voiceOutputMode);
    apply_if_present(update.teamVoiceChannel, target.teamVoiceChannel);
    apply_if_present(update.reservedMode, target.reservedMode);
    apply_if_present(update.migrationVersion, target.migrationVersion);
    apply_if_present(update.chatVolume, target.chatVolume);
    apply_if_present(update.muteWhenUnfocused, target.muteWhenUnfocused);
    apply_if_present(update.soundEffectsVolume, target.soundEffectsVolume);
    apply_if_present(update.dialogueVolume, target.dialogueVolume);
    apply_if_present(update.musicVolume, target.musicVolume);
}

/** Applies the account-backed display fields present in the preference record. */
static void apply_display(const DisplayDelta& update, Display& target) noexcept {
    apply_if_present(update.brightness, target.brightness);
    apply_if_present(update.showFps, target.showFps);
    apply_if_present(update.hdrMode, target.hdrMode);
    apply_if_present(update.calibrationPrimary, target.calibrationPrimary);
    apply_if_present(update.calibrationAlpha, target.calibrationAlpha);
}

/** Applies the supported HUD, subtitle, reticle, and text fields. */
static void apply_interface(const InterfaceDelta& update, Interface& target) noexcept {
    apply_if_present(update.subtitlesMode, target.subtitlesMode);
    apply_if_present(update.colorblindMode, target.colorblindMode);
    apply_if_present(update.helmetMode, target.helmetMode);
    apply_if_present(update.hudOpacity, target.hudOpacity);
    apply_if_present(update.displayHints, target.displayHints);
    apply_if_present(update.backgroundOpacity, target.backgroundOpacity);
    apply_if_present(update.reticleLocation, target.reticleLocation);
    apply_if_present(update.reticleColor, target.reticleColor);
    apply_if_present(update.textSize, target.textSize);
    apply_if_present(update.textColor, target.textColor);
    apply_if_present(update.textBackgroundStyle, target.textBackgroundStyle);
    apply_if_present(update.textBackgroundOpacity, target.textBackgroundOpacity);
    apply_if_present(update.reservedTextMode, target.reservedTextMode);
    apply_if_present(update.subtitleOptionsEntry, target.subtitleOptionsEntry);
}

/** Applies the supported matchmaking, identity, voice, and chat fields. */
static void apply_social(const SocialDelta& update, Social& target) noexcept {
    apply_if_present(update.preferGoodConnection, target.preferGoodConnection);
    apply_if_present(update.textChatMode, target.textChatMode);
    apply_if_present(update.showRealNames, target.showRealNames);
    apply_if_present(update.clanInviteNotifications, target.clanInviteNotifications);
    apply_if_present(update.profanityFilter, target.profanityFilter);
    apply_if_present(update.voiceChatEnabled, target.voiceChatEnabled);
    apply_if_present(update.whisperChatMode, target.whisperChatMode);
    apply_if_present(update.teamChatJoinMode, target.teamChatJoinMode);
    apply_if_present(update.localChatJoinMode, target.localChatJoinMode);
    apply_if_present(update.clanChatJoinMode, target.clanChatJoinMode);
    apply_if_present(update.chatAutoHideMode, target.chatAutoHideMode);
}

/** Applies a sparse update to a local candidate without exposing partial or invalid output. */
bool apply_delta(const SettingsDelta& delta,
                 const AccountSettings& before,
                 AccountSettings& after,
                 bool& changed) noexcept {
    after = {};
    changed = false;
    if (!valid(before)) {
        return false;
    }

    AccountSettings candidate = before;
    apply_controls(delta.controls, candidate.controls);
    apply_audio(delta.audio, candidate.audio);
    apply_display(delta.display, candidate.display);
    apply_interface(delta.interface, candidate.interface);
    apply_social(delta.social, candidate.social);
    apply_if_present(delta.keyBindingSource, candidate.keyBindingSource);
    if (candidate.keyBindingSource == KeyBindingSource::account) {
        apply_if_present(delta.keyBindings, candidate.keyBindings);
    }
    if (!valid(candidate)) {
        return false;
    }

    changed = candidate != before;
    after = candidate;
    return true;
}

} // namespace sunrise::state::account::settings
