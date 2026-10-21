#include "activity_link_selection.h"

#include "../../state/activity/transactions/internal.h"
#include "internal.h"

namespace sunrise::server::bap {
const Session* activity_link_for_generation_locked(const state::activity::SessionBinding& binding,
                                                   std::uint64_t generation,
                                                   std::size_t& count) noexcept {
    count = 0;
    if (!generation || !binding.sessionId || !binding.createdRevision) {
        return nullptr;
    }
    const Session* selected = nullptr;
    for (const auto& session : sessions()) {
        if (!session.id || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration != generation
            || !state::activity::same_binding(session.activity.session, binding)
            || !state::activity::transactions::same_destination(
                session.activity.session.destination, binding.destination)) {
            continue;
        }
        selected = &session;
        ++count;
    }
    return count == 1 ? selected : nullptr;
}
const Session* mission_activity_link_locked(const state::activity::SessionBinding& binding,
                                            std::size_t& count) noexcept {
    count = 0;
    if (!binding.sessionId || !binding.createdRevision) {
        return nullptr;
    }
    const Session* selected = nullptr;
    for (const auto& session : sessions()) {
        if (!session.id || !session.authenticated || !session.activity.bindingGeneration
            || session.activity.role == ActivityClientRole::none
            || !state::activity::same_binding(session.activity.session, binding)
            || !state::activity::transactions::same_destination(
                session.activity.session.destination, binding.destination)
            || (session.activity.role == ActivityClientRole::privateCurrent
                && session.activityJoinGeneration != session.activity.bindingGeneration)) {
            continue;
        }
        // This chooses a BAP delivery route, not the client that simulates the enemies.
        // The oldest joined private route remains stable as more players arrive.
        const bool isPrivate = session.activity.role == ActivityClientRole::privateCurrent;
        const bool selectedPrivate =
            selected != nullptr && selected->activity.role == ActivityClientRole::privateCurrent;
        if (selected == nullptr || (isPrivate && !selectedPrivate)
            || (isPrivate == selectedPrivate
                && session.activity.bindingGeneration < selected->activity.bindingGeneration)) {
            selected = &session;
        }
    }
    return selected == nullptr ? nullptr
                               : activity_link_for_generation_locked(
                                     binding, selected->activity.bindingGeneration, count);
}
} // namespace sunrise::server::bap
