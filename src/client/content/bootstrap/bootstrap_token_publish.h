#pragma once

namespace sunrise::client::content::bootstrap {

/** Publishes the installed client's bootstrap content-id token into State. */
[[nodiscard]] bool publish_token() noexcept;

} // namespace sunrise::client::content::bootstrap
