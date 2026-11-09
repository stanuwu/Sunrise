#pragma once

#include "../../../middleware/content/packages/reader/reader.h"
#include "../items/packages/package_reward_build.h"

namespace sunrise::client::content::vendors {

/**
 * Extracts the vendor catalog from the installed packages, once.
 * The whole index is read, and a definition for every row it names.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the other content passes.
 * @param conditions Loaded condition tables each sale row's availability binds through.
 * @return True when State already holds the catalog or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch,
                         const items::packages::RewardConditions& conditions) noexcept;

} // namespace sunrise::client::content::vendors
