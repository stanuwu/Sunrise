#pragma once

#include <string_view>

#include "definition.h"

namespace sunrise::state::content_manifest {

/**
 * Loads a current cache or extracts one complete installed-package manifest.
 * @param module Loaded Sunrise module, or null to disable cache persistence.
 * @param packagesDirectory Caller-selected installed packages directory.
 * @return True when a checked nonempty catalog is ready in State.
 */
[[nodiscard]] bool initialize(void* module, std::wstring_view packagesDirectory) noexcept;

/** Clears the generated manifest, fingerprints, GUID, and extraction scratch. */
void shutdown() noexcept;

/**
 * Visits one immutable catalog without leaking State-owned pointers after the call.
 * @param visitor Non-reentrant callback that consumes the borrowed catalog.
 * @param context Caller context passed to the callback.
 * @return False when State is not ready or the callback rejects the view.
 */
[[nodiscard]] bool visit_snapshot(SnapshotVisitor visitor, void* context) noexcept;

} // namespace sunrise::state::content_manifest
