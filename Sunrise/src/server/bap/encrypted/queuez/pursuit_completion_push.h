#pragma once

#include <cstddef>
#include <span>

#include "../internal.h"

namespace sunrise::server::bap::encrypted {

/**
 * Redeems and publishes one held pursuit whose contract completes automatically, through the
 * same grant a redemption request uses. A finished sweep is not repeated until saved state or the
 * installed contracts change.
 * @param response Poll output the frame must fit.
 * @param written Receives the frame size.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a frame was written.
 */
[[nodiscard]] bool consume_pursuit_completion(Session& session,
                                              Scratch& scratch,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              bool& touchesScratch) noexcept;

} // namespace sunrise::server::bap::encrypted
