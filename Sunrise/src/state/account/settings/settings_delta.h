#pragma once

#include <cstdint>
#include <optional>

#include "key_bindings.h"

namespace sunrise::state::account::settings {

enum class KeyBindingSource : std::uint8_t;
struct AccountSettings;

/** Sparse controller and mouse settings supplied by one client writeback. */
struct ControlsDelta {
    std::optional<std::int8_t> buttonLayout;
    std::optional<std::int8_t> movementMode;
    std::optional<std::int8_t> controllerLookSensitivity;
    std::optional<bool> controllerInvertVertical;
    std::optional<bool> controllerAutoLookCentering;
    std::optional<bool> controllerVibration;
    std::optional<bool> controllerSwapShoulders;
    std::optional<bool> controllerInvertHorizontal;
    std::optional<std::int32_t> mouseLookSensitivity;
    std::optional<bool> mouseInvertVertical;
    std::optional<bool> mouseInvertHorizontal;
    std::optional<bool> unidentifiedToggle;
    std::optional<bool> mouseAimSmoothing;
    std::optional<float> adsSensitivityModifier;
    std::optional<std::int8_t> doublePressDelay;
};

/** Sparse audio settings supplied by one client writeback. */
struct AudioDelta {
    std::optional<std::int8_t> voiceOutputMode;
    std::optional<std::int8_t> teamVoiceChannel;
    std::optional<std::int8_t> reservedMode;
    std::optional<std::int8_t> migrationVersion;
    std::optional<std::int8_t> chatVolume;
    std::optional<bool> muteWhenUnfocused;
    std::optional<std::int8_t> soundEffectsVolume;
    std::optional<std::int8_t> dialogueVolume;
    std::optional<std::int8_t> musicVolume;
};

/** Sparse display settings supplied by one client writeback. */
struct DisplayDelta {
    std::optional<std::int8_t> brightness;
    std::optional<bool> showFps;
    std::optional<std::int8_t> hdrMode;
    std::optional<float> calibrationPrimary;
    std::optional<float> calibrationAlpha;
};

/** Sparse interface settings supplied by one client writeback. */
struct InterfaceDelta {
    std::optional<std::int8_t> subtitlesMode;
    std::optional<std::int8_t> colorblindMode;
    std::optional<std::int8_t> helmetMode;
    std::optional<std::int8_t> hudOpacity;
    std::optional<bool> displayHints;
    std::optional<std::int8_t> backgroundOpacity;
    std::optional<std::int8_t> reticleLocation;
    std::optional<std::int8_t> reticleColor;
    std::optional<std::int8_t> textSize;
    std::optional<std::int8_t> textColor;
    std::optional<std::int8_t> textBackgroundStyle;
    std::optional<std::int8_t> textBackgroundOpacity;
    std::optional<std::int8_t> reservedTextMode;
    std::optional<std::int8_t> subtitleOptionsEntry;
};

/** Sparse social settings supplied by one client writeback. */
struct SocialDelta {
    std::optional<bool> preferGoodConnection;
    std::optional<std::int8_t> textChatMode;
    std::optional<bool> showRealNames;
    std::optional<bool> clanInviteNotifications;
    std::optional<bool> profanityFilter;
    std::optional<bool> voiceChatEnabled;
    std::optional<std::int8_t> whisperChatMode;
    std::optional<std::int8_t> teamChatJoinMode;
    std::optional<std::int8_t> localChatJoinMode;
    std::optional<std::int8_t> clanChatJoinMode;
    std::optional<std::int8_t> chatAutoHideMode;
};

/**
 * Sparse account-settings writeback plus binding-table routing information.
 *
 * Every scalar uses optional presence so zero and false remain ordinary authored values. The
 * keybinding source is both an authored setting and routing input for the binding-table merge.
 * The keybinding table is one optional fixed-size object; individual slots cannot be partially
 * published by the decoder or committed by State.
 */
struct SettingsDelta {
    ControlsDelta controls;
    AudioDelta audio;
    DisplayDelta display;
    InterfaceDelta interface;
    SocialDelta social;
    std::optional<KeyBindingSource> keyBindingSource;
    std::optional<bindings::KeyBindings> keyBindings;
};

/**
 * Applies only present fields to one complete settings object and validates the result.
 * A binding table is applied only when the resulting binding source is account-backed. A table
 * accompanying computer-local bindings is intentionally ignored because it may be stale.
 * @param delta Sparse client-authored values.
 * @param before Complete authoritative settings before the update.
 * @param after Receives the complete validated candidate; cleared on failure.
 * @param changed Receives whether any supported semantic value differs.
 * @return True when both the input settings and merged candidate are valid.
 */
[[nodiscard]] bool apply_delta(const SettingsDelta& delta,
                               const AccountSettings& before,
                               AccountSettings& after,
                               bool& changed) noexcept;

} // namespace sunrise::state::account::settings
