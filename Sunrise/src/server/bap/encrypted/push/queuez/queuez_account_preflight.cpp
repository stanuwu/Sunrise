#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../internal.h"

namespace sunrise::server::bap::encrypted::push {
namespace {

/** Set once the verdict can no longer change, so later frames skip the state lock. */
std::atomic<bool> g_settled{false};

/**
 * Reports one preflight that left the account uncanonical.
 * @param level Level to report at.
 * @param reason Short skip reason written to the line.
 */
void report(core::log::Level level, const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=queuez stage=account_preflight result=skip reason=%s",
                                      reason);
    if (written > 0) {
        core::log::write(
            core::log::Channel::server, level, {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Canonicalizes the account before any family image is allowed to read it. */
void ensure_account_canonical() noexcept {
    if (state::bound_account() != state::kLocalAccount) {
        return;
    }
    if (g_settled.load(std::memory_order_acquire)) {
        return;
    }
    switch (state::ensure_character_emote_collection()) {
    case state::EmoteCollectionOutcome::ready:
        g_settled.store(true, std::memory_order_release);
        break;
    case state::EmoteCollectionOutcome::unsupported:
        // The installed content decides this and cannot change under a running process.
        g_settled.store(true, std::memory_order_release);
        report(core::log::Level::warn, "unsupported");
        break;
    case state::EmoteCollectionOutcome::notReady:
        // Normal until content extraction and account setup finish; every family still reads the
        // same un-migrated account, so the images agree with each other.
        report(core::log::Level::debug, "not_ready");
        break;
    case state::EmoteCollectionOutcome::failed:
        report(core::log::Level::warn, "failed");
        break;
    }
}

} // namespace sunrise::server::bap::encrypted::push
