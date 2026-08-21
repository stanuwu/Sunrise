#pragma once

#include "domains.h"
#include "format.h"

namespace sunrise::state::build_data::cache::records {

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the row is valid.
 */
[[nodiscard]] bool encode(const content::Definition& value, NamedRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const NamedRecord& record, content::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const items::Definition& value, ItemRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const ItemRecord& record, items::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const collectibles::Definition& value,
                          CollectibleRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return Always true.
 */
[[nodiscard]] bool decode(const CollectibleRecord& record,
                          collectibles::Definition& value) noexcept;

/**
 * Exact dense action-cost set codec.
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the requirement count fits its fixed rows.
 */
[[nodiscard]] bool encode(const material_requirements::Definition& value,
                          MaterialRequirementSetRecord& record) noexcept;

/**
 * Exact dense action-cost set codec.
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the reserved field and every packed boolean are canonical.
 */
[[nodiscard]] bool decode(const MaterialRequirementSetRecord& record,
                          material_requirements::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the state is a known one.
 */
[[nodiscard]] bool encode(const items::details::Definition& value,
                          ItemDetailRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the state byte is a known one.
 */
[[nodiscard]] bool decode(const ItemDetailRecord& record,
                          items::details::Definition& value) noexcept;

/**
 * Exact ordinary-socket rule codec.
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the row's canonical padding is zero.
 */
[[nodiscard]] bool encode(const items::socket_plugs::Rule& value,
                          SocketPlugRuleRecord& record) noexcept;

/**
 * Exact ordinary-socket rule codec.
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the record's reserved byte is zero.
 */
[[nodiscard]] bool decode(const SocketPlugRuleRecord& record,
                          items::socket_plugs::Rule& value) noexcept;

/**
 * Deduplicated socket-pool range codec.
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const items::socket_plugs::Pool& value,
                          SocketPlugPoolRecord& record) noexcept;

/**
 * Deduplicated socket-pool range codec. Cross-row contiguity is checked at the domain boundary.
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return Always true.
 */
[[nodiscard]] bool decode(const SocketPlugPoolRecord& record,
                          items::socket_plugs::Pool& value) noexcept;

/**
 * Flat allowed-plug member codec.
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(items::socket_plugs::Member value,
                          SocketPlugMemberRecord& record) noexcept;

/**
 * Flat allowed-plug member codec.
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return Always true.
 */
[[nodiscard]] bool decode(const SocketPlugMemberRecord& record,
                          items::socket_plugs::Member& value) noexcept;

/**
 * @param value Runtime catalyst relation to pack.
 * @param record Receives the canonical disk form.
 * @return True when the runtime relation has a supported availability value.
 */
[[nodiscard]] bool encode(const items::catalysts::Definition& value,
                          ExoticCatalystRecord& record) noexcept;

/**
 * @param record Canonical disk form to unpack.
 * @param value Receives the runtime catalyst relation.
 * @return True when the disk form has a supported availability value.
 */
[[nodiscard]] bool decode(const ExoticCatalystRecord& record,
                          items::catalysts::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const inventory::buckets::Descriptor& value,
                          InventoryBucketRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return Always true.
 */
[[nodiscard]] bool decode(const InventoryBucketRecord& record,
                          inventory::buckets::Descriptor& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const socket_entry_lists::Definition& value,
                          SocketEntryListRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const SocketEntryListRecord& record,
                          socket_entry_lists::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const socket_entry_lists::EntryTable& value,
                          SocketEntryTableRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const SocketEntryTableRecord& record,
                          socket_entry_lists::EntryTable& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const abilities::Definition& value, AbilityBucketRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when every count is inside its limit.
 */
[[nodiscard]] bool decode(const AbilityBucketRecord& record, abilities::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return Always true.
 */
[[nodiscard]] bool encode(const progressions::Definition& value,
                          ProgressionRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const ProgressionRecord& record,
                          progressions::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the row is valid.
 */
[[nodiscard]] bool encode(const scenarios::Definition& value, ScenarioRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const ScenarioRecord& record, scenarios::Definition& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the row is valid.
 */
[[nodiscard]] bool encode(const scenarios::RosterGroup& value, RosterGroupRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const RosterGroupRecord& record, scenarios::RosterGroup& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the row is valid.
 */
[[nodiscard]] bool encode(const hash_names::Name& value, HashNameRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const HashNameRecord& record, hash_names::Name& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the row is valid.
 */
[[nodiscard]] bool encode(const spawn_sets::Stem& value, SpawnStemRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const SpawnStemRecord& record, spawn_sets::Stem& value) noexcept;

/**
 * @param value Runtime row to pack.
 * @param record Receives the packed disk row.
 * @return True when the row is valid.
 */
[[nodiscard]] bool encode(const spawn_sets::NameHash& value, SpawnNameHashRecord& record) noexcept;

/**
 * @param record Packed disk row.
 * @param value Receives the runtime row.
 * @return True when the disk row is in standard form.
 */
[[nodiscard]] bool decode(const SpawnNameHashRecord& record, spawn_sets::NameHash& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const spawn_sets::Point& value, SpawnPointRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const SpawnPointRecord& record, spawn_sets::Point& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const vendors::IndexEntry& value, VendorIndexRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const VendorIndexRecord& record, vendors::IndexEntry& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const vendors::Definition& value,
                          VendorDefinitionRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const VendorDefinitionRecord& record,
                          vendors::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const vendors::SaleRow& value, VendorSaleRowRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const VendorSaleRowRecord& record, vendors::SaleRow& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const vendors::InstalledRow& value,
                          VendorInstalledRowRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const VendorInstalledRowRecord& record,
                          vendors::InstalledRow& value) noexcept;

} // namespace sunrise::state::build_data::cache::records
