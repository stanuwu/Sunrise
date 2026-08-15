#pragma once

#include "../../../patterns/image_scan.h"

namespace sunrise::client::hooks::network::investment {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Attaches the family-five commit rearm.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_family5_rearm() noexcept;

/** @return True when the family-five commit rearm is absent. */
[[nodiscard]] bool uninstall_family5_rearm() noexcept;

/** @return True while the family-five commit rearm is attached. */
[[nodiscard]] bool family5_rearm_is_installed() noexcept;

/**
 * Arms one derived-state rebuild, used up by the next freshness verdict. Armed twice: when the
 * family-four lookup first returns a real object, and after each family-five commit, which is the
 * first point the account's unlock overrides can be read back.
 */
void arm_derived_rebuild() noexcept;

} // namespace sunrise::client::hooks::network::investment
