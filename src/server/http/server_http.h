#pragma once

#include "../../client/network/consumer.h"

namespace sunrise::server::http {

/** Routes an HTTP request to an in-process Server handler. */
[[nodiscard]] bool consume(const client::network::HttpRequest& request,
                           client::network::HttpResponse& response) noexcept;

} // namespace sunrise::server::http
