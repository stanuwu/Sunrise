#include <bitset>

#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Parses matchmaking and chat settings under stable Sunrise-owned names. */
bool Parser::social_settings(state::account::settings::Social& output) noexcept {
    enum class Field : std::size_t {
        preferGoodConnection,
        textChatMode,
        showRealNames,
        clanInviteNotifications,
        profanityFilter,
        voiceChatEnabled,
        whisperChatMode,
        teamChatJoinMode,
        localChatJoinMode,
        clanChatJoinMode,
        chatAutoHideMode,
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
        if (key == "prefer_good_connection") {
            if (!mark(Field::preferGoodConnection) || !boolean(output.preferGoodConnection)) {
                return false;
            }
        } else if (key == "text_chat_mode") {
            if (!mark(Field::textChatMode) || !signed_byte(output.textChatMode)) {
                return false;
            }
        } else if (key == "show_real_names") {
            if (!mark(Field::showRealNames) || !boolean(output.showRealNames)) {
                return false;
            }
        } else if (key == "clan_invite_notifications") {
            if (!mark(Field::clanInviteNotifications) || !boolean(output.clanInviteNotifications)) {
                return false;
            }
        } else if (key == "profanity_filter") {
            if (!mark(Field::profanityFilter) || !boolean(output.profanityFilter)) {
                return false;
            }
        } else if (key == "voice_chat_enabled") {
            if (!mark(Field::voiceChatEnabled) || !boolean(output.voiceChatEnabled)) {
                return false;
            }
        } else if (key == "whisper_chat_mode") {
            if (!mark(Field::whisperChatMode) || !signed_byte(output.whisperChatMode)) {
                return false;
            }
        } else if (key == "team_chat_join_mode") {
            if (!mark(Field::teamChatJoinMode) || !signed_byte(output.teamChatJoinMode)) {
                return false;
            }
        } else if (key == "local_chat_join_mode") {
            if (!mark(Field::localChatJoinMode) || !signed_byte(output.localChatJoinMode)) {
                return false;
            }
        } else if (key == "clan_chat_join_mode") {
            if (!mark(Field::clanChatJoinMode) || !signed_byte(output.clanChatJoinMode)) {
                return false;
            }
        } else if (key == "chat_auto_hide_mode") {
            if (!mark(Field::chatAutoHideMode) || !signed_byte(output.chatAutoHideMode)) {
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
