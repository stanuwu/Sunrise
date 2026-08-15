#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::matchmaking {

/**
 * Acquires the first available generation-checked logical context.
 * @param context Receives the acquired handle.
 * @return True when one of the 4 context slots is free.
 */
[[nodiscard]] bool acquire_context(ContextHandle& context) noexcept;

/**
 * Releases a context and securely erases every descriptor it owns.
 * @param context Handle returned by acquire_context.
 * @return True when the active generation matched and was released.
 */
[[nodiscard]] bool release_context(ContextHandle context) noexcept;

/**
 * Prepares a variant update without changing stored State.
 * @param context Active logical matchmaking context.
 * @param requestId Requested nonzero id, or zero to take one from State.
 * @param variant Variant key used by the per-context advertisement table.
 * @param hasDescriptor True when descriptor holds the exact protocol descriptor.
 * @param descriptor Descriptor bytes that must stay valid until commit returns.
 * @param advertisementId Receives the chosen nonzero id.
 * @param mutation Receives the data the commit checks against.
 * @return True when the update can be committed against the captured revisions.
 */
[[nodiscard]] bool prepare_variant_update(ContextHandle context,
                                          std::uint64_t requestId,
                                          std::uint64_t variant,
                                          bool hasDescriptor,
                                          std::span<const std::byte> descriptor,
                                          std::uint64_t& advertisementId,
                                          PendingMutation& mutation) noexcept;

/**
 * Finds the latest id, or prepares the first standalone id.
 * @param context Active logical matchmaking context.
 * @param advertisementId Receives the chosen nonzero id.
 * @param mutation Cleared, or a prepared transaction when no latest id exists.
 * @return True when a latest id exists or can be committed.
 */
[[nodiscard]] bool prepare_initial_latest(ContextHandle context,
                                          std::uint64_t& advertisementId,
                                          PendingMutation& mutation) noexcept;

/**
 * Copies the latest advertisement under the State lock.
 * @param context Active logical matchmaking context.
 * @param snapshot Cleared securely. Call erase_snapshot after encoding it.
 * @return True when the context has a latest advertisement.
 */
[[nodiscard]] bool latest_snapshot(ContextHandle context, LatestSnapshot& snapshot) noexcept;

/**
 * Commits one prepared mutation only when all captured revisions still match.
 * @param mutation Prepared data, always cleared before this function returns.
 * @return True when no mutation was needed, or the mutation committed in one step.
 */
[[nodiscard]] bool commit(PendingMutation& mutation) noexcept;

/**
 * Securely erases a transient latest-advertisement snapshot.
 * @param snapshot Snapshot to erase after its descriptor has been encoded.
 */
void erase_snapshot(LatestSnapshot& snapshot) noexcept;

} // namespace sunrise::state::matchmaking
