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
} // namespace sunrise::server::bap
