#pragma once

namespace sunrise::client::hooks::exception_probe {

/**
 * Logs C++ throws from this DLL, faults in its code and access violations in game code, with the
 * throw type and return addresses. It only observes; every exception goes on to the next handler.
 */
bool install() noexcept;

/** Removes the handler. */
bool uninstall() noexcept;

} // namespace sunrise::client::hooks::exception_probe
