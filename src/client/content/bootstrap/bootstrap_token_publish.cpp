#include "bootstrap_token_publish.h"

#include <Windows.h>

#include <array>
#include <cstddef>

#include "../../../core/logging/log.h"
#include "../../../state/runtime/runtime.h"
#include "../../targets/game.h"

namespace sunrise::client::content::bootstrap {

/** Copies the bootstrap content-id token out of the installed client into State. */
bool publish_token() noexcept {
    const std::byte* const source = targets::game::network::get().contentIdToken;
    if (source == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootstrap stage=token result=fail reason=target");
        return false;
    }
    std::array<std::byte, state::kBootstrapTokenSize> token{};
    __try {
        for (std::size_t index = 0; index < token.size(); ++index) {
            token[index] = source[index];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootstrap stage=token result=fail reason=read");
        return false;
    }
    if (!state::publish_bootstrap_token(token)) {
        SecureZeroMemory(token.data(), token.size());
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootstrap stage=token result=fail reason=publish");
        return false;
    }
    SecureZeroMemory(token.data(), token.size());
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=bootstrap stage=token result=ok");
    return true;
}

} // namespace sunrise::client::content::bootstrap
