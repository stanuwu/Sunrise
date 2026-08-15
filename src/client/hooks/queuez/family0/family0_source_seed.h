#pragma once

#include <cstddef>

namespace sunrise::client::hooks::queuez::family0 {

/** The client's own source-list getter, a thunk decoded from the sweep's call operand. */
using SourceList = std::byte*(__fastcall*)();

/** @param getter The source-list getter we found, or null to withdraw it. */
void publish_source_list(SourceList getter) noexcept;

/**
 * Captures the account key the seed needs. Runs on the sweep's thread.
 * @param manager Borrowed queuez manager.
 */
void learn_account_key(std::byte* manager) noexcept;

/** Seeds the source list with the account key, unless the first entry is already ours. */
void seed_source_list() noexcept;

/** Clears the captured key, the published getter, and the one-shot report. */
void reset() noexcept;

} // namespace sunrise::client::hooks::queuez::family0
