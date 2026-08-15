#pragma once

#include "../../../../state/investment/investment.h"
#include "../../../encoding/bit_writer.h"

namespace sunrise::middleware::web_service::messages::family5 {

/**
 * Validates every bounded family-5 field before encoding.
 * @return True when counts, slots, values, and object identity are safe.
 */
[[nodiscard]] bool valid(const state::Family5State& family) noexcept;

/**
 * Writes all 10 family-5 fields in descriptor order.
 * @param writer Fixed-buffer payload writer.
 * @param family Must already have passed valid().
 * @return True when the whole nested object fits.
 */
[[nodiscard]] bool write(encoding::bits::Writer& writer,
                         const state::Family5State& family) noexcept;

} // namespace sunrise::middleware::web_service::messages::family5
