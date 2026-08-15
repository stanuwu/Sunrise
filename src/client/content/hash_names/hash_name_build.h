#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::hash_names {

/**
 * Finds the internal name of every destination bubble it can, once.
 * A bubble stores only the hash of its name, so names are recovered by hashing the ids the
 * installed objects carry. Runs after the destination layouts publish, which give the hashes.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the item build.
 * @return True when State already holds the table or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch) noexcept;

} // namespace sunrise::client::content::hash_names
