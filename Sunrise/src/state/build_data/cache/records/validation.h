#pragma once

#include "../../definition.h"
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

/**
 * Checks exact socket rules and members against the complete item and detail domains.
 * @param rules Exact item and lane rules.
 * @param pools Deduplicated plug pools.
 * @param members Flat plug indices owned by the pools.
 * @param itemDefinitions Complete dense item rows.
 * @param details Configured item details.
 * @return True when every target lane exists and every plug index names an installed item row.
 */
[[nodiscard]] bool
valid_socket_plug_links(std::span<const items::socket_plugs::Rule> rules,
                        std::span<const items::socket_plugs::Pool> pools,
                        std::span<const items::socket_plugs::Member> members,
                        std::span<const items::Definition> itemDefinitions,
                        std::span<const items::details::Definition> details) noexcept;

/**
 * Checks catalyst item, socket, plug, lifecycle, and pinned release links.
 * @param build Cache build identity.
 * @param domains Complete cache domains.
 * @return True when a supported build matches exactly, or an unsupported build has no catalog.
 */
[[nodiscard]] bool valid_exotic_catalyst_links(const BuildIdentity& build,
                                               Domains domains) noexcept;

/**
 * Checks every collectible item index against the complete dense item table.
 * @param collectibleDefinitions Complete dense collectible rows.
 * @param itemDefinitions Complete dense item rows.
 * @return True when each row has its own ordinal and every available item link exists.
 */
[[nodiscard]] bool
valid_collectible_links(std::span<const collectibles::Definition> collectibleDefinitions,
                        std::span<const items::Definition> itemDefinitions) noexcept;

/**
 * @param build Cache build identity.
 * @param domains Complete sorted domains.
 * @return True when every domain required by the build passes its checks.
 */
[[nodiscard]] bool valid_domains(const BuildIdentity& build, Domains domains) noexcept;

} // namespace sunrise::state::build_data::cache::records
