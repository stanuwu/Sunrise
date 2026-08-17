#pragma once

#include <string_view>

namespace sunrise::client::content::entity_names {

/** Creates the package entity name cache when no current cache exists. */
[[nodiscard]] bool ensure(std::wstring_view packageDirectory) noexcept;

} // namespace sunrise::client::content::entity_names
