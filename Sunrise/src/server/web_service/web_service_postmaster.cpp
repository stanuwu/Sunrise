/** Postmaster claims the web service prepares from one opcode-405 request. */

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode405.h"
#include "../../state/runtime/runtime.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {

/** Parses and prepares one opcode-405 Postmaster claim. */
void claim_postmaster_item(const middleware::web_service::Message& message,
                           Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode405::Request request{};
    if (!middleware::web_service::messages::opcode405::parse_request(message, request)
        || request.vendorIndex < 0 || request.sourceBucket < 0 || request.definitionIndex < 0
        || request.quantity <= 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws405 stage=request result=refused");
        return;
    }
    auto* mutation = emplace_mutation<state::PendingPostmasterClaim>(outcome);
    const char* reason = "storage";
    const bool prepared =
        mutation
        && state::prepare_postmaster_claim(static_cast<std::uint16_t>(request.vendorIndex),
                                           static_cast<std::uint8_t>(request.sourceBucket),
                                           request.instanceSoid,
                                           static_cast<std::uint16_t>(request.definitionIndex),
                                           request.quantity,
                                           *mutation,
                                           reason);
    if (prepared) {
        return;
    }
    clear_mutation(outcome);
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=ws405 stage=prepare result=refused reason=%s vendor=%d bucket=%d "
                      "item=%d instance=0x%llX quantity=%d",
                      reason,
                      request.vendorIndex,
                      request.sourceBucket,
                      request.definitionIndex,
                      static_cast<unsigned long long>(request.instanceSoid),
                      request.quantity);
}

} // namespace sunrise::server::web_service
