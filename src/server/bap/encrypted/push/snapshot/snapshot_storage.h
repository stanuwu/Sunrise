#pragma once

#include "snapshot.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Prior live payload prefixes that a new preparation may not overwrite. */
struct Reservation {
    std::size_t rawWriteOffset{};
    std::size_t compressedWriteOffset{};
    std::size_t rawClearSize{};
    std::size_t compressedClearSize{};
};

/**
 * Finds payload prefixes borrowed by a caller's earlier prepared snapshot.
 * @param scratch Raw and compressed snapshot storage owned by the lock.
 * @param prepared Caller output that may still hold a live prepared snapshot.
 * @return Checked write offsets and carried-over cleanup extents, or an empty reservation.
 */
[[nodiscard]] Reservation reserve_prior(const Scratch& scratch, const Prepared& prepared) noexcept;

/**
 * Wipes the staging tails but keeps a reserved prior snapshot valid.
 * @param scratch Raw and compressed snapshot storage owned by the lock.
 * @param reservation Prefixes whose live payload bytes must survive rejection.
 */
void clear_after(Scratch& scratch, const Reservation& reservation) noexcept;

/**
 * Publishes a finished snapshot and re-points its object span at the caller's storage.
 * @param staged Finished internal snapshot whose descriptors borrow scratch buffers.
 * @param output Gets the snapshot only when the staged storage is valid.
 * @return True when the staged object view belongs to its own descriptor array.
 */
[[nodiscard]] bool commit(const Prepared& staged, Prepared& output) noexcept;

} // namespace sunrise::server::bap::encrypted::push::snapshot
