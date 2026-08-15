#pragma once

#include <span>

#include "definition.h"

namespace sunrise::state::content_manifest {

/** @param row Candidate public package row. @return True when its disk fields are canonical. */
[[nodiscard]] bool valid(const Row& row) noexcept;

/**
 * Checks one catalog sorted by unique file name. A public package id may repeat.
 * @param rows Complete candidate catalog.
 * @return True when every row is canonical and the catalog is nonempty.
 */
[[nodiscard]] bool valid(std::span<const Row> rows) noexcept;

} // namespace sunrise::state::content_manifest
