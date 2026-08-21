#include "settings_state.h"

#include <array>
#include <cstddef>

namespace sunrise::state::account::settings {
namespace {

template <typename Value> struct Range {
    Value minimum;
    Value maximum;
};

/** The controller layout menu publishes these 7 stored values. */
constexpr std::array<std::int8_t, 7> kButtonLayouts{0, 1, 2, 3, 5, 6, 9};
/** The movement-layout menu stores 4 choices, 0 to 3. */
constexpr Range<std::int8_t> kMovementModes{0, 3};
/** Controller sensitivity stores the 10 menu choices as 0 to 9. */
constexpr Range<std::int8_t> kControllerSensitivity{0, 9};
/** Mouse sensitivity accepts the menu's 1 to 100 scale. */
constexpr Range<std::int32_t> kMouseSensitivity{1, 100};
/** ADS sensitivity stores the menu's 0.5 through 1.5 multiplier. */
constexpr Range<float> kAdsSensitivity{0.5F, 1.5F};
/** Double-press delay stores the 5 menu choices, 0 to 4. */
constexpr Range<std::int8_t> kDoublePressDelay{0, 4};

/** Two-choice selectors store only the menu values 0 and 1. */
constexpr Range<std::int8_t> kTwoChoiceSelector{0, 1};
/** Voice output stores 3 menu choices, 0 to 2. */
constexpr Range<std::int8_t> kVoiceOutputMode{0, 2};
/** Chat volume exposes 9 menu levels, 0 to 8. */
constexpr Range<std::int8_t> kChatVolume{0, 8};
/** Sound, dialogue and music expose 11 levels, 0 to 10. */
constexpr Range<std::int8_t> kAudioVolume{0, 10};

/** Brightness stores the 7 menu choices, 0 to 6. */
constexpr Range<std::int8_t> kBrightness{0, 6};
/** VSync stores the DXGI presentation interval from 0 to 4. */
constexpr Range<std::uint8_t> kVerticalSyncInterval{0, 4};
/** Field of view accepts the game's full 55 through 155 degree range. */
constexpr Range<std::int32_t> kFieldOfView{55, 155};
/** The first unidentified calibration field uses the working renderer fallback. */
constexpr float kCalibrationPrimary = 10000.0F;
/** The second unidentified calibration field uses the working renderer alpha. */
constexpr float kCalibrationAlpha = 0.0F;

/** Subtitle mode stores 3 menu choices, 0 to 2. */
constexpr Range<std::int8_t> kSubtitlesMode{0, 2};
/** Colorblind mode stores 4 menu choices, 0 to 3. */
constexpr Range<std::int8_t> kColorblindMode{0, 3};
/** HUD opacity stores 4 menu choices, 0 to 3. */
constexpr Range<std::int8_t> kHudOpacity{0, 3};
/** Background opacity stores 5 menu choices, 0 to 4. */
constexpr Range<std::int8_t> kBackgroundOpacity{0, 4};
/** Reticle color stores 7 menu choices, 0 to 6. */
constexpr Range<std::int8_t> kReticleColor{0, 6};
/** Text size stores 5 menu choices, 0 to 4. */
constexpr Range<std::int8_t> kTextSize{0, 4};
/** Text color and background style each store 4 choices, 0 to 3. */
constexpr Range<std::int8_t> kTextPresentationMode{0, 3};
/** Unidentified text settings stay on their only known working value. */
constexpr std::int8_t kUnidentifiedTextValue = 0;
/** Text chat stores 4 menu choices, 0 to 3. */
constexpr Range<std::int8_t> kTextChatMode{0, 3};

/**
 * Tests one scalar against an inclusive Sunrise settings policy range.
 * @param range Inclusive supported range.
 * @return True when the value is inside the range.
 */
template <typename Value> [[nodiscard]] bool within(Value value, Range<Value> range) noexcept {
    return value >= range.minimum && value <= range.maximum;
}

/**
 * Tests one scalar against a Sunrise settings policy domain with gaps.
 * @param values Every supported value.
 * @return True when the candidate is one of them.
 */
template <typename Value, std::size_t Count>
[[nodiscard]] bool contains(const std::array<Value, Count>& values, Value candidate) noexcept {
    for (const Value value : values) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

/**
 * Checks controller and mouse values against their authored menu domains.
 * @return True when every number belongs to its supported domain.
 */
[[nodiscard]] bool valid_controls(const Controls& value) noexcept {
    return contains(kButtonLayouts, value.buttonLayout)
           && within(value.movementMode, kMovementModes)
           && within(value.controllerLookSensitivity, kControllerSensitivity)
           && within(value.mouseLookSensitivity, kMouseSensitivity)
           && within(value.adsSensitivityModifier, kAdsSensitivity)
           && within(value.doublePressDelay, kDoublePressDelay);
}

/**
 * Checks voice and volume values against their authored menu domains.
 * @return True when every number belongs to its supported domain.
 */
[[nodiscard]] bool valid_audio(const Audio& value) noexcept {
    return within(value.voiceOutputMode, kVoiceOutputMode)
           && within(value.teamVoiceChannel, kTwoChoiceSelector)
           && within(value.reservedMode, kTwoChoiceSelector)
           && value.migrationVersion == kCompletedAudioMigrationVersion
           && within(value.chatVolume, kChatVolume)
           && within(value.soundEffectsVolume, kAudioVolume)
           && within(value.dialogueVolume, kAudioVolume) && within(value.musicVolume, kAudioVolume);
}

/**
 * Checks renderer values against the supported choices and working calibration values.
 * @return True when every number belongs to its supported domain.
 */
[[nodiscard]] bool valid_display(const Display& value) noexcept {
    return within(value.brightness, kBrightness) && within(value.hdrMode, kTwoChoiceSelector)
           && within(value.verticalSyncInterval, kVerticalSyncInterval)
           && within(value.fieldOfView, kFieldOfView)
           && value.calibrationPrimary == kCalibrationPrimary
           && value.calibrationAlpha == kCalibrationAlpha;
}

/**
 * Checks HUD and text values against their authored menu domains.
 * @return True when every number belongs to its supported domain.
 */
[[nodiscard]] bool valid_interface(const Interface& value) noexcept {
    return within(value.subtitlesMode, kSubtitlesMode)
           && within(value.colorblindMode, kColorblindMode)
           && within(value.helmetMode, kTwoChoiceSelector) && within(value.hudOpacity, kHudOpacity)
           && within(value.backgroundOpacity, kBackgroundOpacity)
           && within(value.reticleLocation, kTwoChoiceSelector)
           && within(value.reticleColor, kReticleColor) && within(value.textSize, kTextSize)
           && within(value.textColor, kTextPresentationMode)
           && within(value.textBackgroundStyle, kTextPresentationMode)
           && within(value.textBackgroundOpacity, kBackgroundOpacity)
           && value.reservedTextMode == kUnidentifiedTextValue
           && value.subtitleOptionsEntry == kUnidentifiedTextValue;
}

/**
 * Checks chat values against their authored menu domains.
 * @return True when every number belongs to its supported domain.
 */
[[nodiscard]] bool valid_social(const Social& value) noexcept {
    return within(value.textChatMode, kTextChatMode)
           && within(value.whisperChatMode, kTwoChoiceSelector)
           && within(value.teamChatJoinMode, kTwoChoiceSelector)
           && within(value.localChatJoinMode, kTwoChoiceSelector)
           && within(value.clanChatJoinMode, kTwoChoiceSelector)
           && within(value.chatAutoHideMode, kTwoChoiceSelector);
}

} // namespace

/** Checks a whole account-settings object against the supported menu domains. */
bool valid(const AccountSettings& value) noexcept {
    const bool validBindingSource = value.keyBindingSource == KeyBindingSource::account
                                    || value.keyBindingSource == KeyBindingSource::computer;
    return value.configured && value.keyBindings.configured && validBindingSource
           && valid_controls(value.controls) && valid_audio(value.audio)
           && valid_display(value.display) && valid_interface(value.interface)
           && valid_social(value.social);
}

} // namespace sunrise::state::account::settings
