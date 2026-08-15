#include "config_getter_answers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string_view>

#include "../../../core/settings/settings.h"
#include "../../../state/content_manifest/content_manifest_state_runtime.h"
#include "../network/content_config/protocol.h"

namespace sunrise::client::hooks::config_getter {
namespace {

/** The Client copies the token as a fixed 37-byte field. */
constexpr std::size_t kTokenSize = state::content_manifest::kGuidCapacity;
static_assert(kTokenSize == 37);
/** The getter reports a supplied value with 1. */
constexpr char kSupplied = 1;

/** Process-lifetime token storage; the Client keeps the returned pointer past the call. */
std::array<char, kTokenSize> g_token{};
std::atomic<bool> g_tokenReady{};

/**
 * Copies the manifest identity out of one borrowed State view.
 * @param context Token storage.
 * @param view Immutable manifest view.
 * @return True when the identity is exactly the fixed length.
 */
[[nodiscard]] bool copy_guid(void* context, const state::content_manifest::View& view) noexcept {
    auto* output = static_cast<std::array<char, kTokenSize>*>(context);
    if (view.guid.size() != kTokenSize - 1) {
        return false;
    }
    std::copy(view.guid.begin(), view.guid.end(), output->begin());
    output->back() = '\0';
    return true;
}

/** Fills the token once from State, or from the external server's authored GUID. */
[[nodiscard]] bool ensure_token() noexcept {
    if (g_tokenReady.load(std::memory_order_acquire)) {
        return true;
    }
    const core::settings::client::external::Settings& external =
        core::settings::get().client.externalServer;
    if (external.enabled) {
        // The external server compares this against field 5 of the manifest it serves, so the
        // generated identity is the wrong value here.
        std::copy(external.configGuid.begin(), external.configGuid.end(), g_token.begin());
    } else if (!state::content_manifest::visit_snapshot(&copy_guid, &g_token)) {
        return false;
    }
    g_tokenReady.store(true, std::memory_order_release);
    return true;
}

/**
 * Answers the config URL question with the route the Client's own fetch then requests.
 * @param self Native config singleton, unused.
 * @param output Caller storage the Client sizes for a full URL.
 * @return 1 when the URL is supplied.
 */
char __fastcall url_body([[maybe_unused]] void* self, char* output) noexcept {
    if (output == nullptr) {
        return 0;
    }
    const core::settings::client::external::Settings& external =
        core::settings::get().client.externalServer;
    const std::string_view url = external.enabled
                                     ? std::string_view(external.configUrl.data())
                                     : ::sunrise::client::hooks::network::content_config::kLocalUrl;
    std::copy(url.begin(), url.end(), output);
    output[url.size()] = '\0';
    return kSupplied;
}

/**
 * Answers the config token question. The Client copies 37 bytes and later compares them against
 * the manifest identity field, so this must be the same value.
 * @param self Native config singleton, unused.
 * @return Token storage, or null while the manifest is not yet published.
 */
void* __fastcall token_body([[maybe_unused]] void* self) noexcept {
    return ensure_token() ? static_cast<void*>(g_token.data()) : nullptr;
}

} // namespace

/** @return Direct internal-linkage config URL getter body. */
void* url_entry_point() noexcept {
    return reinterpret_cast<void*>(&url_body);
}

/** @return Direct internal-linkage config token getter body. */
void* token_entry_point() noexcept {
    return reinterpret_cast<void*>(&token_body);
}

} // namespace sunrise::client::hooks::config_getter
