#include <bitset>

#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Parses voice, volume, and migration settings under stable Sunrise-owned names. */
bool Parser::audio_settings(state::account::settings::Audio& output) noexcept {
    enum class Field : std::size_t {
        voiceOutputMode,
        teamVoiceChannel,
        reservedMode,
        migrationVersion,
        chatVolume,
        muteWhenUnfocused,
        soundEffectsVolume,
        dialogueVolume,
        musicVolume,
        count,
    };

    output = {};
    if (!consume('{')) {
        return false;
    }
    std::bitset<static_cast<std::size_t>(Field::count)> supplied;
    const auto mark = [&supplied](Field field) noexcept {
        const std::size_t index = static_cast<std::size_t>(field);
        if (supplied.test(index)) {
            return false;
        }
        supplied.set(index);
        return true;
    };
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "voice_output_mode") {
            if (!mark(Field::voiceOutputMode) || !signed_byte(output.voiceOutputMode)) {
                return false;
            }
        } else if (key == "team_voice_channel") {
            if (!mark(Field::teamVoiceChannel) || !signed_byte(output.teamVoiceChannel)) {
                return false;
            }
        } else if (key == "reserved_mode") {
            if (!mark(Field::reservedMode) || !signed_byte(output.reservedMode)) {
                return false;
            }
        } else if (key == "migration_version") {
            if (!mark(Field::migrationVersion) || !signed_byte(output.migrationVersion)) {
                return false;
            }
        } else if (key == "chat_volume") {
            if (!mark(Field::chatVolume) || !signed_byte(output.chatVolume)) {
                return false;
            }
        } else if (key == "mute_when_unfocused") {
            if (!mark(Field::muteWhenUnfocused) || !boolean(output.muteWhenUnfocused)) {
                return false;
            }
        } else if (key == "sound_effects_volume") {
            if (!mark(Field::soundEffectsVolume) || !signed_byte(output.soundEffectsVolume)) {
                return false;
            }
        } else if (key == "dialogue_volume") {
            if (!mark(Field::dialogueVolume) || !signed_byte(output.dialogueVolume)) {
                return false;
            }
        } else if (key == "music_volume") {
            if (!mark(Field::musicVolume) || !signed_byte(output.musicVolume)) {
                return false;
            }
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return supplied.all();
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
