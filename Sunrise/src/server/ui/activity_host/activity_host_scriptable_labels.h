#pragma once

#include <cstdint>

#include "../../../state/build_data/scriptables/definition.h"

namespace sunrise::server::ui::activity_host::scriptable_labels {

/** @return Stable UI text for one catalog build status. */
[[nodiscard]] const char* status(state::build_data::scriptables::BuildStatus value) noexcept;

/** @return Stable UI text for one package-presence classification. */
[[nodiscard]] const char* presence(state::build_data::scriptables::GroupSafety value) noexcept;

/** @return Stable UI text for one package-name evidence tier. */
[[nodiscard]] const char* provenance(state::build_data::scriptables::NameProvenance value) noexcept;

/** @return Stable UI text for one registry descriptor scope. */
[[nodiscard]] const char* scope(std::uint16_t descriptor) noexcept;

/**
 * Tests one registry descriptor against a scope combo entry.
 * @param scope Combo index: 0 or out of range is every scope, then shared, registry, state-local.
 */
[[nodiscard]] bool scope_matches(std::uint16_t descriptor, int scope) noexcept;

} // namespace sunrise::server::ui::activity_host::scriptable_labels
