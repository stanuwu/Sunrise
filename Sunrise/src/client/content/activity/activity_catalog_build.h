#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::activity {

/**
 * Extracts and publishes optional launcher data once on the investment worker.
 * @param source Installed package source; no file is created or modified.
 * @param scratch Worker-owned reader storage, reused only by this caller.
 * @return True when the immutable catalog is ready; failure settles the optional startup work.
 */
[[nodiscard]] bool build_catalog(const middleware::content::packages::reader::Source& source,
                                 middleware::content::packages::reader::Scratch& scratch) noexcept;
} // namespace sunrise::client::content::activity
