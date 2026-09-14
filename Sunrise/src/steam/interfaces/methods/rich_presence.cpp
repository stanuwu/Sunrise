#include <array>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../state/steam/steam_state.h"
#include "../internal.h"

namespace sunrise::steam::interfaces::methods {
namespace {

namespace presence = sunrise::state::steam;

} // namespace

/**
 * Retains one rich-presence pair for the local user.
 * @param key Exact Client key, such as `status`, `connect` or `steam_player_group`.
 * @param value Exact Client value. An empty value drops the key.
 * @return True once the pair is retained. False makes the Client's publication aggregate fail.
 */
bool set_rich_presence([[maybe_unused]] void* self, const char* key, const char* value) noexcept {
    const bool stored = presence::set_rich_presence(text_view(key), text_view(value));
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     stored ? "ev=steam stage=rich_presence result=ok"
                            : "ev=steam stage=rich_presence result=refuse");
    return stored;
}

/** Drops every retained rich-presence pair for the local user. */
void clear_rich_presence([[maybe_unused]] void* self) noexcept {
    presence::clear_rich_presence();
}

/**
 * Serves a retained rich-presence value back.
 * @param steamId Friend asked about. Only the local user has retained presence here.
 * @return Retained value, or an empty string. Never null.
 */
const char*
friend_rich_presence([[maybe_unused]] void* self, std::uint64_t steamId, const char* key) noexcept {
    return steamId == local_steam_id() ? presence::rich_presence_value(text_view(key)) : "";
}

/** @return Retained pair count for the local user, and zero for every other identity. */
int friend_rich_presence_key_count([[maybe_unused]] void* self, std::uint64_t steamId) noexcept {
    return steamId == local_steam_id() ? presence::rich_presence_key_count() : 0;
}

/**
 * Serves a retained rich-presence key by table position.
 * @return Retained key, or an empty string. Never null.
 */
const char*
friend_rich_presence_key([[maybe_unused]] void* self, std::uint64_t steamId, int index) noexcept {
    return steamId == local_steam_id() ? presence::rich_presence_key(index) : "";
}

} // namespace sunrise::steam::interfaces::methods
