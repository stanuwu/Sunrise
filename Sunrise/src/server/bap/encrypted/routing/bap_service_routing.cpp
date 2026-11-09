#include "../internal.h"

namespace sunrise::server::bap::encrypted::routing {

/**
 * Maps an authenticated request service to its response codec.
 * @param request Numeric request service from the decrypted inner header.
 * @param route Gets the response contract.
 * @return True when the encrypted service is implemented.
 */
bool resolve(std::uint16_t request, ServiceRoute& route) noexcept {
    using Request = middleware::bap::RequestService;
    using Response = middleware::bap::ResponseService;
    switch (static_cast<Request>(request)) {
    case Request::activityHostManager:
        route = {ResponseMode::reply,
                 Response::activityHostManager,
                 BodyCodec::activityHostManagerResponse};
        return true;
    case Request::activityMessage:
        route = {ResponseMode::uncorrelatedPush, {}, BodyCodec::activityMessageRequest};
        return true;
    case Request::webService:
        route = {ResponseMode::reply, Response::webService, BodyCodec::webService};
        return true;
    case Request::webServiceServer:
        route = {ResponseMode::reply, Response::webServiceServer, BodyCodec::webService};
        return true;
    case Request::notification29:
        route = {ResponseMode::none, {}, BodyCodec::empty};
        return true;
    case Request::subscribeFamily:
        route = {ResponseMode::reply, Response::subscribeFamily, BodyCodec::familySubscription};
        return true;
    case Request::unsubscribeFamily:
        route = {ResponseMode::reply, Response::unsubscribeFamily, BodyCodec::familyUnsubscription};
        return true;
    case Request::activityHost:
        route = {ResponseMode::reply, Response::activityHost, BodyCodec::activityHostResponse};
        return true;
    case Request::clientConfig:
        route = {ResponseMode::reply, Response::clientConfig, BodyCodec::clientConfigResponse};
        return true;
    case Request::purchasedOffers:
        route = {ResponseMode::reply, Response::purchasedOffers, BodyCodec::empty};
        return true;
    case Request::accountTranslation:
        route = {ResponseMode::reply,
                 Response::accountTranslation,
                 BodyCodec::accountTranslationResponse};
        return true;
    // These five need a reply: each holds the head of the Client's pending queue until one comes.
    // Every field of their response bodies is optional, so an empty body is valid.
    case Request::skill:
        route = {ResponseMode::reply, Response::skill, BodyCodec::empty};
        return true;
    case Request::request36:
        route = {ResponseMode::reply, Response::response37, BodyCodec::empty};
        return true;
    case Request::request38:
        route = {ResponseMode::reply, Response::response39, BodyCodec::empty};
        return true;
    case Request::request40:
        route = {ResponseMode::reply, Response::response41, BodyCodec::empty};
        return true;
    case Request::request48:
        route = {ResponseMode::reply, Response::response49, BodyCodec::empty};
        return true;
    case Request::request50:
        route = {ResponseMode::reply, Response::response51, BodyCodec::empty};
        return true;
    case Request::matchmaking:
        route = {ResponseMode::reply, Response::matchmaking, BodyCodec::matchmakingResponse};
        return true;
    case Request::clan:
        route = {ResponseMode::reply, Response::clan, BodyCodec::empty};
        return true;
    case Request::registerSubscriber:
        route = {ResponseMode::reply, Response::registerSubscriber, BodyCodec::empty};
        return true;
    case Request::notification171:
        route = {ResponseMode::none, {}, BodyCodec::empty};
        return true;
    case Request::echo:
        route = {ResponseMode::reply, Response::echo, BodyCodec::empty};
        return true;
    case Request::registerRelayClient:
        route = {
            ResponseMode::reply, Response::registerRelayClient, BodyCodec::registerRelayClient};
        return true;
    case Request::initiateRelayConnection:
        route = {ResponseMode::uncorrelatedPush, {}, BodyCodec::initiateRelayConnection};
        return true;
    case Request::signSteamCertificate:
        route = {ResponseMode::reply, Response::signSteamCertificate, BodyCodec::steamCertificate};
        return true;
    case Request::accountFromMembership:
        route = {ResponseMode::reply, Response::accountFromMembership, BodyCodec::empty};
        return true;
    case Request::userMessage:
        route = {ResponseMode::reply, Response::userMessage, BodyCodec::userMessageResponse};
        return true;
    default:
        // Unknown services stay quiet. Failing the send would drop the whole BAP link.
        // Quiet is only safe when the service has no response id. A request needs a case above.
        route = {ResponseMode::none, {}, BodyCodec::empty};
        return true;
    }
}

} // namespace sunrise::server::bap::encrypted::routing
