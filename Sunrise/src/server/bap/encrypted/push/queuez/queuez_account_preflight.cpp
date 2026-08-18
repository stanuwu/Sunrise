#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../internal.h"

namespace sunrise::server::bap::encrypted::push {
namespace {

/**
 * Set once the answer can no longer change within this process, so the common path costs one
 * relaxed load rather than a lock and a whole account copy on every pushed frame.
 */
std::atomic<bool> g_settled{false};

/**
 * Reports a preflight that left the account uncanonical, naming which of the two reasons it was.
 * Silence here would be indistinguishable from a migration that ran, which is the confusion this
 * whole preflight exists to remove.
 */
void report(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=queuez stage=account_preflight result=skip reason=%s",
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Canonicalizes the account before any family image is allowed to read it. */
void ensure_account_canonical() noexcept {
    if (g_settled.load(std::memory_order_acquire)) {
        return;
    }
    switch (state::ensure_character_emote_collection()) {
    case state::EmoteCollectionOutcome::ready:
        g_settled.store(true, std::memory_order_release);
        break;
    case state::EmoteCollectionOutcome::unsupported:
        // The installed content decides this one and cannot change under a running process, so
        // the verdict is final. Reported once rather than on every frame that follows.
        g_settled.store(true, std::memory_order_release);
        report("unsupported");
        break;
    case state::EmoteCollectionOutcome::notReady:
        // Content extraction or account setup has not finished. Every family reads the same
        // un-migrated account meanwhile, so they still agree with each other.
        report("not_ready");
        break;
    case state::EmoteCollectionOutcome::failed:
        report("failed");
        break;
    }
}

} // namespace sunrise::server::bap::encrypted::push
