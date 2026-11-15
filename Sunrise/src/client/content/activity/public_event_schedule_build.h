#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::activity {
/**
 * Extracts the Director's public-event schedule table (investment_globals child 2) and publishes
 * it to state::build_data::public_event_schedule.
 * @param source Installed package directory with borrowed block keys.
 * @param scratch Reader-owned block storage.
 * @return True once published or the complete named catalog has no supported schedule.
 * A package read failure leaves the pass pending.
 */
[[nodiscard]] bool
build_public_event_schedule(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch) noexcept;
} // namespace sunrise::client::content::activity
