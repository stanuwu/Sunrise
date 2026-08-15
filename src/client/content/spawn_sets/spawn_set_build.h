#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::spawn_sets {

/**
 * Extracts the spawn-set catalogue from the installed packages, once.
 * Rows are grouped by map-package stem, which is the key a destination row carries. Each row also
 * carries the mask of the bubbles that offer the set.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the item build.
 * @return True when State already holds the catalogue or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch) noexcept;

} // namespace sunrise::client::content::spawn_sets
