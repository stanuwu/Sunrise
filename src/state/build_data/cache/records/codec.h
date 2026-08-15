#pragma once

#include "domains.h"
#include "format.h"

namespace sunrise::state::build_data::cache::records {

/** @param record Receives the packed disk row. @return True when the row is valid. */
[[nodiscard]] bool encode(const content::Definition& value, NamedRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const NamedRecord& record, content::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const items::Definition& value, ItemRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const ItemRecord& record, items::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return True when the state is a known one. */
[[nodiscard]] bool encode(const items::details::Definition& value,
                          ItemDetailRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the state byte is a known one. */
[[nodiscard]] bool decode(const ItemDetailRecord& record,
                          items::details::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const inventory::buckets::Descriptor& value,
                          InventoryBucketRecord& record) noexcept;

/** @param value Receives the runtime row. @return Always true. */
[[nodiscard]] bool decode(const InventoryBucketRecord& record,
                          inventory::buckets::Descriptor& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const socket_entry_lists::Definition& value,
                          SocketEntryListRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const SocketEntryListRecord& record,
                          socket_entry_lists::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const socket_entry_lists::EntryTable& value,
                          SocketEntryTableRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const SocketEntryTableRecord& record,
                          socket_entry_lists::EntryTable& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const abilities::Definition& value, AbilityBucketRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when every count is inside its limit. */
[[nodiscard]] bool decode(const AbilityBucketRecord& record, abilities::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return Always true. */
[[nodiscard]] bool encode(const progressions::Definition& value,
                          ProgressionRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const ProgressionRecord& record,
                          progressions::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return True when the row is valid. */
[[nodiscard]] bool encode(const scenarios::Definition& value, ScenarioRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const ScenarioRecord& record, scenarios::Definition& value) noexcept;

/** @param record Receives the packed disk row. @return True when the row is valid. */
[[nodiscard]] bool encode(const scenarios::RosterGroup& value, RosterGroupRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const RosterGroupRecord& record, scenarios::RosterGroup& value) noexcept;

/** @param record Receives the packed disk row. @return True when the row is valid. */
[[nodiscard]] bool encode(const hash_names::Name& value, HashNameRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const HashNameRecord& record, hash_names::Name& value) noexcept;

/** @param record Receives the packed disk row. @return True when the row is valid. */
[[nodiscard]] bool encode(const spawn_sets::Stem& value, SpawnStemRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const SpawnStemRecord& record, spawn_sets::Stem& value) noexcept;

/** @param record Receives the packed disk row. @return True when the row is valid. */
[[nodiscard]] bool encode(const spawn_sets::NameHash& value, SpawnNameHashRecord& record) noexcept;

/** @param value Receives the runtime row. @return True when the disk row is in standard form. */
[[nodiscard]] bool decode(const SpawnNameHashRecord& record, spawn_sets::NameHash& value) noexcept;

} // namespace sunrise::state::build_data::cache::records
