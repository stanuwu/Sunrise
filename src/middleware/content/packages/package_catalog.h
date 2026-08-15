#pragma once

#include <string_view>

#include "named_tags.h"

namespace sunrise::middleware::content::packages {

/**
 * Rebuilds generated State mappings from every package in one installed directory.
 * @param directory Package directory with or without a trailing separator.
 * @param result Receives package and named-entry totals.
 * @return True when extraction finishes. On failure the generated catalogue is left empty.
 */
[[nodiscard]] bool load_catalog(std::wstring_view directory,
                                named_tags::DirectoryResult& result) noexcept;

} // namespace sunrise::middleware::content::packages
