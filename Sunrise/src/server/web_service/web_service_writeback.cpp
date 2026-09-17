#include <array>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode702.h"
#include "../../state/account/account_context.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {
/** Stages public joinability and native presence; the enclosing BAP request owns publication. */
bool note_character_writeback(
    const middleware::web_service::Message& message,
    std::span<const state::account::inventory::PresentedItemRow> presentation,
    Outcome& outcome) noexcept {
    if (state::bound_account() != state::kLocalAccount) {
        return false;
    }
    namespace writeback = middleware::web_service::messages::opcode702;
    writeback::Request request{};
    const bool parsed = writeback::parse_request(message, request);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=writeback result=%s join_lock_flags=%u",
                                      parsed ? "ok" : "unparsed",
                                      static_cast<unsigned>(request.joinLockFlags));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    std::uint64_t selected{};
    if (!parsed || !state::local_selected_character_soid(selected)
        || (request.newItems
            && !state::account::inventory::record_character_seen(*request.newItems,
                                                                 presentation))) {
        return false;
    }
    request.presence.characterSoid = selected;
    outcome.nativePresence = request.presence;
    return true;
}

} // namespace sunrise::server::web_service
