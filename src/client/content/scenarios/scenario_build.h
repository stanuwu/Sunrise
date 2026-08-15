#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::scenarios {

/**
 * Extracts every destination's bubble layout from the installed packages, once.
 * Each destination costs one tag read, because activity message 1 reads only the scenario blob.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the item build.
 * @return True when State already holds the domain or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch) noexcept;

} // namespace sunrise::client::content::scenarios
