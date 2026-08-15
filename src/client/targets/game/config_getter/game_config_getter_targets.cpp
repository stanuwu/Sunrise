#include "game_config_getter_targets.h"

#include <array>

#include "../../../patterns/game/config_getter/config_getter_signature_bytes.h"
#include "../config_getter.h"

namespace sunrise::client::targets::game::config_getter {
namespace {

Targets g_targets;
bool g_resolved{};

} // namespace

/** Derives both config getter thunks. */
bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept {
    output = {};
    const std::array<patterns::Pattern, 2> definitions{
        patterns::Pattern{"config_url_getter", patterns::game::config_getter::kUrlGetter},
        patterns::Pattern{"config_token_getter", patterns::game::config_getter::kTokenGetter},
    };
    std::array<patterns::Match, 2> matches{};
    if (!patterns::resolve_all(image, definitions, matches)) {
        return false;
    }
    for (const patterns::Match match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }

    Targets resolved;
    resolved.url = matches[0].address;
    resolved.token = matches[1].address;
    output = resolved;
    return true;
}

/** @param targets Validated config getter table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
    g_resolved = true;
}

/** Clears the published config getter group. */
void clear() noexcept {
    g_targets = {};
    g_resolved = false;
}

/** @return Process-local config getter group. */
const Targets& get() noexcept {
    return g_targets;
}

/** @return True after both thunks are published. */
bool is_resolved() noexcept {
    return g_resolved;
}

} // namespace sunrise::client::targets::game::config_getter
