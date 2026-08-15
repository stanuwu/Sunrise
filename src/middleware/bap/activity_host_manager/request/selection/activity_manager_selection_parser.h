#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {

/**
 * Takes the optional safe field-one scalars, checking the whole protobuf structure. Other
 * fields are walked only to prove their encoded bounds.
 * @param input Complete declared protobuf bytes from a checked service-6 request.
 * @param result Cleared first. Receives only scalar selection data on success.
 * @return True for a supported protobuf, including an absent or empty field one.
 */
[[nodiscard]] bool parse_selection(std::span<const std::byte> input,
                                   ActivityManagerSelectionResult& result) noexcept;

} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
