#include "activity_patch_epoch_route.h"

#include "../../../../../middleware/bap/activity_message/activity_patch_epoch_parser.h"

namespace sunrise::server::bap::encrypted::activity_message::patch_epoch {

namespace message = middleware::bap::activity_message::patch_epoch;

/** Parses type 52 and keeps its epoch for the next roster update. */
bool prepare(std::uint64_t sessionId,
             const middleware::bap::activity_message::Request& request,
             ActivityPlan& plan) noexcept {
    message::PatchEpoch epoch{};
    if (!message::parse_patch_epoch(request.payload, epoch)) {
        return false;
    }
    plan.patchEpoch = epoch;
    plan.sessionId = sessionId;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::patchEpoch;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message::patch_epoch
