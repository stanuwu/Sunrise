#include "../internal.h"

namespace sunrise::server::bap::encrypted::routing {

/**
 * Maps an authenticated request service to its response codec and metadata.
 * @param request Numeric request service from the decrypted inner header.
 * @param route Gets the response and structured-log metadata.
 * @return True when the encrypted service is implemented.
 */
bool resolve(std::uint16_t request, ServiceRoute& route) noexcept {
    switch (static_cast<middleware::bap::RequestService>(request)) {
    case middleware::bap::RequestService::activityHostManager:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::activityHostManager,
                 BodyCodec::activityHostManagerResponse,
                 "ev=bap svc=6 rsp=7 result=ok"};
        return true;
    case middleware::bap::RequestService::activityMessage:
        route = {ResponseMode::uncorrelatedPush,
                 {},
                 BodyCodec::activityMessageRequest,
                 "ev=bap svc=8 rsp=none result=ok"};
        return true;
    case middleware::bap::RequestService::webService:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::webService,
                 BodyCodec::webService,
                 "ev=bap svc=10 rsp=11 result=ok"};
        return true;
    case middleware::bap::RequestService::webServiceServer:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::webServiceServer,
                 BodyCodec::webService,
                 "ev=bap svc=110 rsp=112 result=ok"};
        return true;
    case middleware::bap::RequestService::notification29:
        route = {ResponseMode::none, {}, BodyCodec::empty, "ev=bap svc=29 rsp=none result=ok"};
        return true;
    case middleware::bap::RequestService::subscribeFamily:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::subscribeFamily,
                 BodyCodec::familySubscription,
                 "ev=bap svc=12 rsp=13 result=ok"};
        return true;
    case middleware::bap::RequestService::unsubscribeFamily:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::unsubscribeFamily,
                 BodyCodec::familyUnsubscription,
                 "ev=bap svc=14 rsp=15 result=ok"};
        return true;
    case middleware::bap::RequestService::activityHost:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::activityHost,
                 BodyCodec::activityHostResponse,
                 "ev=bap svc=16 rsp=17 result=ok"};
        return true;
    case middleware::bap::RequestService::clientConfig:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::clientConfig,
                 BodyCodec::clientConfigResponse,
                 "ev=bap svc=18 rsp=19 result=ok"};
        return true;
    case middleware::bap::RequestService::purchasedOffers:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::purchasedOffers,
                 BodyCodec::empty,
                 "ev=bap svc=21 rsp=22 result=ok"};
        return true;
    case middleware::bap::RequestService::accountTranslation:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::accountTranslation,
                 BodyCodec::accountTranslationResponse,
                 "ev=bap svc=23 rsp=24 result=ok"};
        return true;
    // These five need a reply: each holds the head of the Client's pending queue until one comes.
    // Every field of their response bodies is optional, so an empty body is valid.
    case middleware::bap::RequestService::skill:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::skill,
                 BodyCodec::empty,
                 "ev=bap svc=34 rsp=35 result=ok"};
        return true;
    case middleware::bap::RequestService::request36:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::response37,
                 BodyCodec::empty,
                 "ev=bap svc=36 rsp=37 result=ok"};
        return true;
    case middleware::bap::RequestService::request38:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::response39,
                 BodyCodec::empty,
                 "ev=bap svc=38 rsp=39 result=ok"};
        return true;
    case middleware::bap::RequestService::request40:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::response41,
                 BodyCodec::empty,
                 "ev=bap svc=40 rsp=41 result=ok"};
        return true;
    case middleware::bap::RequestService::request48:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::response49,
                 BodyCodec::empty,
                 "ev=bap svc=48 rsp=49 result=ok"};
        return true;
    case middleware::bap::RequestService::matchmaking:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::matchmaking,
                 BodyCodec::matchmakingResponse,
                 "ev=bap svc=42 rsp=43 result=ok"};
        return true;
    case middleware::bap::RequestService::clan:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::clan,
                 BodyCodec::empty,
                 "ev=bap svc=44 rsp=45 result=ok"};
        return true;
    case middleware::bap::RequestService::registerSubscriber:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::registerSubscriber,
                 BodyCodec::empty,
                 "ev=bap svc=121 rsp=122 result=ok"};
        return true;
    case middleware::bap::RequestService::notification171:
        route = {ResponseMode::none, {}, BodyCodec::empty, "ev=bap svc=171 rsp=none result=ok"};
        return true;
    case middleware::bap::RequestService::echo:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::echo,
                 BodyCodec::empty,
                 "ev=bap svc=250 rsp=251 result=ok"};
        return true;
    case middleware::bap::RequestService::registerRelayClient:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::registerRelayClient,
                 BodyCodec::empty,
                 "ev=bap svc=302 rsp=303 result=ok"};
        return true;
    case middleware::bap::RequestService::signSteamCertificate:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::signSteamCertificate,
                 BodyCodec::steamCertificate,
                 "ev=bap svc=304 rsp=305 result=ok"};
        return true;
    case middleware::bap::RequestService::accountFromMembership:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::accountFromMembership,
                 BodyCodec::empty,
                 "ev=bap svc=306 rsp=307 result=ok"};
        return true;
    case middleware::bap::RequestService::userMessage:
        route = {ResponseMode::reply,
                 middleware::bap::ResponseService::userMessage,
                 BodyCodec::userMessageResponse,
                 "ev=bap svc=32 rsp=33 result=ok"};
        return true;
    default:
        // Unknown services stay quiet. Failing the send would drop the whole BAP link.
        // Quiet is only safe when the service has no response id. A request needs a case above.
        route = {
            ResponseMode::none, {}, BodyCodec::empty, "ev=bap svc=unhandled rsp=none result=ok"};
        return true;
    }
}

} // namespace sunrise::server::bap::encrypted::routing
