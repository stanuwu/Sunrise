#pragma once

namespace sunrise::client::content::diagnostics {

/** Reports build-data domain readiness, once per change. */
void report_readiness() noexcept;

} // namespace sunrise::client::content::diagnostics
