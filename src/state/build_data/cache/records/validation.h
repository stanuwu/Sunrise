#pragma once

#include "domains.h"

namespace sunrise::state::build_data::cache::records {

/**
 * Sorts the filled rows into the only accepted disk order.
 * @param domains Mutable fixed storage for every domain.
 * @param counts Filled row counts inside each storage span.
 * @return True when every count fits its storage span.
 */
[[nodiscard]] bool canonicalize(MutableDomains domains, const DomainCounts& counts) noexcept;

/**
 * Checks the configured details against dense items, character buckets, and socket lists.
 * @param details Details that passed their own checks, or an empty detail domain.
 * @param itemDefinitions Complete dense item rows.
 * @param inventoryBuckets Complete bucket-routing rows in bucket order.
 * @param socketEntryLists Complete dense socket-list rows.
 * @return True when every detail reference points inside the other domains.
 */
[[nodiscard]] bool
valid_item_detail_links(std::span<const items::details::Definition> details,
                        std::span<const items::Definition> itemDefinitions,
                        std::span<const inventory::buckets::Descriptor> inventoryBuckets,
                        std::span<const socket_entry_lists::Definition> socketEntryLists) noexcept;

/** @param domains Complete sorted domains. @return True when every domain passes its checks. */
[[nodiscard]] bool valid_domains(Domains domains) noexcept;

} // namespace sunrise::state::build_data::cache::records
