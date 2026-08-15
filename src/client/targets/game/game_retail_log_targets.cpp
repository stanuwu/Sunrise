#include "../../patterns/game.h"
#include "resolution/internal.h"
#include "retail_log.h"

namespace sunrise::client::targets::game::retail_log {
namespace {

Targets g_targets;
bool g_resolved{};

/** @param id Retail-log signature identity. @return Group-local match index. */
[[nodiscard]] constexpr std::size_t index(patterns::game::Id id) noexcept {
    return static_cast<std::size_t>(id) - resolution::kRetailLogFirstMatch;
}

} // namespace

/** Derives the retail-log target table without publishing it. */
bool derive(std::span<const patterns::Match> matches, Targets& output) noexcept {
    output = {};
    if (matches.size() != resolution::kRetailLogMatchCount) {
        return false;
    }
    for (const patterns::Match match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }

    Targets resolved;
    resolved.enqueue = matches[index(patterns::game::Id::retailLogEnqueue)].address;
    resolved.setCategoryVerbosity =
        matches[index(patterns::game::Id::retailLogSetCategoryVerbosity)].address;
    output = resolved;
    return true;
}

/** @param targets Fully validated retail-log table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
    g_resolved = true;
}

/** Clears the published retail-log target group. */
void clear() noexcept {
    g_targets = {};
    g_resolved = false;
}

/** @return Process-local retail-log target group. */
const Targets& get() noexcept {
    return g_targets;
}

/** @return True after both retail-log targets are published. */
bool is_resolved() noexcept {
    return g_resolved;
}

} // namespace sunrise::client::targets::game::retail_log
