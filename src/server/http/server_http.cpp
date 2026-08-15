#include "server_http.h"

#include <chrono>
#include <cstdint>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/signon/response.h"
#include "../../state/entitlements/entitlement_runtime.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::http {
namespace {

/** URL marker owned by the in-process SignOn route. Any query string may follow it. */
constexpr std::string_view kSignOnPath = "/SignOn";
/** HTTP success status written to the Client result prefix. */
constexpr unsigned kHttpOk = 200;

} // namespace

/** Encodes the generated SignOn state for the supported HTTP route. */
bool consume(const client::network::HttpRequest& request,
             client::network::HttpResponse& response) noexcept {
    if (request.url.find(kSignOnPath) == std::string_view::npos) {
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto& signOnState = state::sign_on();
    const auto serverTime = static_cast<std::uint64_t>(now);
    const std::uint64_t expiry = serverTime + signOnState.tokenLifetimeSeconds;
    if (!middleware::signon::encode_success(signOnState,
                                            state::entitlements::get(),
                                            expiry,
                                            serverTime,
                                            request.response,
                                            response.size)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=http method=post route=signon stage=encode result=fail");
        return false;
    }
    response.statusCode = kHttpOk;
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     "ev=http method=post route=signon result=ok");
    return true;
}

} // namespace sunrise::server::http
