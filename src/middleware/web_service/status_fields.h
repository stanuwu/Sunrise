#pragma once

#include "../encoding/bit_writer.h"
#include "web_service_envelope.h"

namespace sunrise::middleware::web_service::status {

/**
 * Writes one whole logical status shape, without envelope trailers.
 * @param writer Fixed-buffer MSB-first payload writer.
 * @param shape Descriptor layout to encode.
 * @param response Logical values before descriptor biases.
 * @return True when every required field fits.
 */
[[nodiscard]] bool write_fields(encoding::bits::Writer& writer,
                                ResponseShape shape,
                                const StatusResponse& response) noexcept;

} // namespace sunrise::middleware::web_service::status
