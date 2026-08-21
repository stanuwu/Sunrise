#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::nodes {

/**
 * Keeps the presentation node table across restarts, in a file of its own.
 *
 * The build data cache does not carry this domain, and the extraction pass only runs when the
 * investment refresh gate finds something missing. Nodes are not in that gate, so a warm start ran
 * with an empty node table: no category was counted and no progress bar could move, whatever had
 * been claimed. Adding nodes to either gate is worse than the bug -- one is inert because it only
 * governs cache writes, and the other retries the package pass forever because nodes do not publish
 * on that path -- so the domain is persisted beside the claim file instead. Neither gate changes,
 * and a warm start simply finds the table already published.
 */

/**
 * Derives the node file path and publishes any table already held.
 * A missing file is not a failure: it is a first run, and extraction will write one.
 * @param module Loaded DLL, used to find the artifact directory.
 * @return True when the path resolves. Loading is best effort and reported separately.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/**
 * Reads the held table and publishes it, if there is one.
 * Kept apart from initialize because a publish is only accepted once the build data runtime is up.
 * @return True when a table was published.
 */
[[nodiscard]] bool load_and_publish() noexcept;

/**
 * Writes the node table so the next start does not need the package pass to rebuild it.
 * @param definitions Complete rows in native node order, as published.
 * @return True when the file is written whole.
 */
[[nodiscard]] bool store(std::span<const Definition> definitions) noexcept;

} // namespace sunrise::state::build_data::nodes
