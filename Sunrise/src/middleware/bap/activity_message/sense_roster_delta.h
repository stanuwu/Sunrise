#pragma once

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::bap::activity_message::sense_update {

/**
 * Skips the roster-delta body that a sense update mirrors back.
 * @param reader Positioned after the delta's root bit; advanced past the body.
 * @return False when the bits do not follow the schema.
 */
[[nodiscard]] bool skip_roster_delta(encoding::bits::Reader& reader) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sense_update
