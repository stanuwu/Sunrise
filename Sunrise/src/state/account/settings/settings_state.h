#pragma once

#include <cstdint>

#include "key_bindings.h"

namespace sunrise::state::account::settings {

/** The account record's one-time audio migration treats version 8 as finished. */
inline constexpr std::int8_t kCompletedAudioMigrationVersion = 8;

/** Selects account-replicated or computer-local keyboard and mouse bindings. */
enum class KeyBindingSource : std::uint8_t {
    account,
    computer,
};

/** Authored controller and mouse input preferences. */
struct Controls {
    std::int8_t buttonLayout{};
    std::int8_t movementMode{};
    std::int8_t controllerLookSensitivity{};
    bool controllerInvertVertical{};
    bool controllerAutoLookCentering{};
    bool controllerVibration{};
    bool controllerSwapShoulders{};
    bool controllerInvertHorizontal{};
    std::int32_t mouseLookSensitivity{};
    bool mouseInvertVertical{};
    bool mouseInvertHorizontal{};
    /**
     * WS-701 preference field 36: a replicated Boolean stored between controller vibration and
     * mouse aim smoothing in the native preference record. Its exact user-facing purpose has not
     * been identified, so Sunrise preserves it for lossless round trips without guessing a name.
     */
    bool unidentifiedToggle{};
    bool mouseAimSmoothing{};
    float adsSensitivityModifier{};
    std::int8_t doublePressDelay{};

    bool operator==(const Controls&) const = default;
};

/** Authored voice and volume preferences. */
struct Audio {
    std::int8_t voiceOutputMode{};
    std::int8_t teamVoiceChannel{};
    /** Kept audio mode with no localized title in the target build. */
    std::int8_t reservedMode{};
    std::int8_t migrationVersion{};
    std::int8_t chatVolume{};
    bool muteWhenUnfocused{};
    std::int8_t soundEffectsVolume{};
    std::int8_t dialogueVolume{};
    std::int8_t musicVolume{};

    bool operator==(const Audio&) const = default;
};

/** Authored screen and renderer preferences. */
struct Display {
    std::int8_t brightness{};
    bool showFps{};
    std::int8_t hdrMode{};
    /** Presentation interval: 0 disables VSync, 1 to 4 wait that many refreshes. */
    std::uint8_t verticalSyncInterval{};
    /** Horizontal field of view in degrees. */
    std::int32_t fieldOfView{85};
    /** First unidentified renderer-calibration scalar. */
    float calibrationPrimary{};
    /** Second unidentified renderer-calibration scalar. */
    float calibrationAlpha{};

    bool operator==(const Display&) const = default;
};

/** Authored HUD, subtitle, reticle, and text presentation preferences. */
struct Interface {
    std::int8_t subtitlesMode{};
    std::int8_t colorblindMode{};
    std::int8_t helmetMode{};
    std::int8_t hudOpacity{};
    bool displayHints{};
    std::int8_t backgroundOpacity{};
    std::int8_t reticleLocation{};
    std::int8_t reticleColor{};
    std::int8_t textSize{};
    std::int8_t textColor{};
    std::int8_t textBackgroundStyle{};
    std::int8_t textBackgroundOpacity{};
    /** Kept text mode with no localized title in the target build. */
    std::int8_t reservedTextMode{};
    std::int8_t subtitleOptionsEntry{};

    bool operator==(const Interface&) const = default;
};

/** Authored matchmaking, identity, voice, and chat preferences. */
struct Social {
    bool preferGoodConnection{};
    std::int8_t textChatMode{};
    bool showRealNames{};
    bool clanInviteNotifications{};
    bool profanityFilter{};
    bool voiceChatEnabled{};
    std::int8_t whisperChatMode{};
    std::int8_t teamChatJoinMode{};
    std::int8_t localChatJoinMode{};
    std::int8_t clanChatJoinMode{};
    std::int8_t chatAutoHideMode{};

    bool operator==(const Social&) const = default;
};

/** Complete authored account-setting values, independent of their native record layout. */
struct AccountSettings {
    Controls controls;
    Audio audio;
    Display display;
    Interface interface;
    Social social;
    KeyBindingSource keyBindingSource{KeyBindingSource::computer};
    bindings::KeyBindings keyBindings;
    /** True only when a settings object was supplied by configuration. */
    bool configured{};

    bool operator==(const AccountSettings&) const = default;
};

/**
 * Checks a whole account-settings object against the supported menu domains.
 * @return True when every needed group is configured and safe to encode.
 */
[[nodiscard]] bool valid(const AccountSettings& value) noexcept;

} // namespace sunrise::state::account::settings
