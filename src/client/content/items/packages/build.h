#pragma once

namespace sunrise::client::content::items::packages {

/**
 * Publishes the dense item table from the installed packages, once.
 * @return True when State already holds the table or a full pass publishes it.
 */
[[nodiscard]] bool build() noexcept;

/**
 * Reports whether the pass can read anything yet.
 * It needs the bootstrap token and the installed key table, both published before the Client
 * worker starts. Until both are there, every call returns at once.
 * @return True when the block keys the pass borrows are there.
 */
[[nodiscard]] bool readable() noexcept;

} // namespace sunrise::client::content::items::packages
