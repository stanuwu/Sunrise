#include "store_internal.h"

namespace sunrise::state::investment::store {
namespace {

/** A reward is owned by the character that was selected when it was earned. */
int selected() noexcept {
    for (std::size_t slot = 0; slot < g_session.selected.size(); ++slot) {
        if (g_session.selected[slot]) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

} // namespace

/** Reward ownership and quantities are committed before presentation is attempted. */
bool enqueue_reward(std::uint32_t definitionHash, std::int32_t quantity) noexcept {
    const std::lock_guard lock(g_mutex);
    const int owner = selected();
    if (owner < 0) {
        return false;
    }
    Statement row("INSERT INTO pending_rewards(character_slot,definition_hash,quantity) "
                  "VALUES (?,?,?)");
    return row.write(owner, definitionHash, quantity);
}

/** Other characters' earned rewards remain queued; only a clean SQL step proves no row follows. */
bool next_reward(PendingReward& output, std::uint64_t afterId, bool& readFailed) noexcept {
    const std::lock_guard lock(g_mutex);
    output = {};
    Statement row("SELECT id,definition_hash,quantity FROM pending_rewards "
                  "WHERE character_slot=? AND id>? ORDER BY id LIMIT 1");
    const int step = row.parameters(selected(), afterId) ? row.step() : SQLITE_ERROR;
    const bool found =
        step == SQLITE_ROW && row.columns(output.id, output.definitionHash, output.quantity);
    readFailed = !found && step != SQLITE_DONE;
    return found;
}

/** The inventory grant and deletion of this reward must share one outer transaction. */
bool complete_reward(std::uint64_t id) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement row("DELETE FROM pending_rewards WHERE id=? AND character_slot=?");
    return row.write(id, selected()) && sqlite3_changes(g_database) == 1;
}

} // namespace sunrise::state::investment::store
