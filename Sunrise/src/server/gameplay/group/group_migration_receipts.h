#pragma once

#include <cstdint>

#include "../../../middleware/encoding/bit_reader.h"

namespace sunrise::server::gameplay::group::migration {

/**
 * Reads one host-migration or election message and records what it said.
 * Nothing acts on one. An unread body leaves the container behind it unreadable, and an echoed one
 * can leave two peers each believing they carry the group.
 * @param id Registry message id.
 * @param reader Reader positioned at the body.
 * @return True when the id is one of these messages and its body was completely read.
 */
[[nodiscard]] bool consume(std::uint8_t id, middleware::encoding::bits::Reader& reader) noexcept;

} // namespace sunrise::server::gameplay::group::migration
