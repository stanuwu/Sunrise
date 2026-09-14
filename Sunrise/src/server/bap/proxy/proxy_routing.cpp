#include "proxy_routing.h"

#include <memory>
#include <new>

#include "../../../core/settings/settings.h"
#include "../../../middleware/bap/account_translation/account_translation_response.h"
#include "../../../middleware/bap/family_subscription.h"
#include "../../../middleware/bap/family_unsubscription.h"
#include "../../../middleware/bap/frame.h"
#include "../../../middleware/web_service/web_service_envelope.h"
#include "../../../state/account/account_platform.h"
#include "../../../state/runtime/runtime.h"

namespace sunrise::server::bap::proxy {

bool is_local_root(std::uint64_t soid) noexcept {
    if (soid == 0) {
        return false;
    }
    if (soid == state::account_primary_soid(state::kLocalAccount)) {
        return true;
    }
    static std::unique_ptr<state::AccountState> local;
    if (!local) {
        local.reset(new (std::nothrow) state::AccountState{});
    }
    if (!local || !state::account_snapshot(state::kLocalAccount, *local)) {
        return false;
    }
    for (std::size_t index = 0; index < local->characterCount && index < local->characters.size();
         ++index) {
        if (local->characters[index].soid == soid) {
            return true;
        }
    }
    return false;
}

Plane classify(std::uint16_t service, std::span<const std::byte> body) noexcept {
    if (!core::settings::get().server.upstream.enabled) {
        return Plane::local;
    }
    using middleware::bap::RequestService;
    switch (static_cast<RequestService>(service)) {
    case RequestService::clientConfig:    // 18
    case RequestService::purchasedOffers: // 21
    case RequestService::clan:            // 44
    case RequestService::serverHello:     // 25 -- plaintext-layer only in practice
    case RequestService::start:           // 30 -- plaintext-layer only in practice
    case RequestService::echo:
    case RequestService::notification29:
    case RequestService::notification171:
    case RequestService::registerSubscriber:
    case RequestService::signSteamCertificate:
    // Character writeback also owns private new-item receipts. Only its committed public
    // presence projection crosses to the session service, through the profile publisher.
    case RequestService::webService:
    case RequestService::webServiceServer:
    case RequestService::skill:
        return Plane::local;

    case RequestService::accountTranslation: {
        std::uint64_t identity = 0;
        if (!middleware::bap::account_translation::request_identity(body, identity)) {
            // Can't tell -- unlike svc 12/14 (session-plane, defaults upstream when unparsable),
            // svc 23 is account-plane by default, so an unreadable body stays LOCAL: it can only
            // ever be a malformed ask about THIS instance's own account/served table.
            return Plane::local;
        }
        const std::uint64_t ownPlatformId = core::settings::get().steam.user.steamId;
        return identity == ownPlatformId ? Plane::local : Plane::upstream;
    }
    case RequestService::subscribeFamily:
    case RequestService::unsubscribeFamily: {
        std::uint8_t family = 0;
        std::uint64_t root = 0;
        if (service == static_cast<std::uint16_t>(RequestService::subscribeFamily)) {
            middleware::queuez::Subscription subscription{};
            if (!middleware::bap::family_subscription::parse(body, subscription)) {
                return Plane::upstream; // Can't tell; a session-plane request defaults upstream.
            }
            family = static_cast<std::uint8_t>(subscription.familyType);
            root = subscription.familyRootSoid;
        } else {
            middleware::bap::family_unsubscription::Request request{};
            if (!middleware::bap::family_unsubscription::parse(body, request)) {
                return Plane::upstream;
            }
            family = request.familyType;
            root = request.familyRootSoid;
        }
        if (family == 5 || ((family == 0 || family == 3 || family == 4) && is_local_root(root))) {
            return Plane::local;
        }
        return Plane::upstream;
    }

    case RequestService::accountProjection:
        return Plane::local;
    default:
        break;
    }
    return Plane::upstream;
}

} // namespace sunrise::server::bap::proxy
