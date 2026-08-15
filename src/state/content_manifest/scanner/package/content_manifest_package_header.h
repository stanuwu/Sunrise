#pragma once

#include <string_view>

#include "../internal.h"

namespace sunrise::state::content_manifest::scanner::package {

/**
 * Reads one unchanged package header and copies its public build signature.
 * @param directory Installed packages directory.
 * @param candidate Listed file whose size, time and package id must still match.
 * @return True when the required header fields match.
 */
[[nodiscard]] bool read_header(std::wstring_view directory, Candidate& candidate) noexcept;

} // namespace sunrise::state::content_manifest::scanner::package
