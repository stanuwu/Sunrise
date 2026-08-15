#pragma once

#include <cstdint>

#include "../../middleware/web_service/web_service_envelope.h"

namespace sunrise::server::web_service {

/**
 * Finds reusable stateless layouts outside the special message codecs.
 * Every opcode gets one. An unlisted one takes the correlated echo.
 * @param opcode Request opcode from the Web Service envelope.
 * @param shape Gets the reusable response layout.
 */
void resolve_response_shape(std::uint16_t opcode,
                            middleware::web_service::ResponseShape& shape) noexcept;

} // namespace sunrise::server::web_service
