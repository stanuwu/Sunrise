#pragma once

#include "domains.h"
#include "format.h"

namespace sunrise::state::build_data::cache::records {
[[nodiscard]] bool encode(const gameplay::entity_object_types::Row&, ObjectTypeRecord&) noexcept;
[[nodiscard]] bool decode(const ObjectTypeRecord&, gameplay::entity_object_types::Row&) noexcept;

[[nodiscard]] bool encode(const gameplay::entity_position_profiles::Row&,
                          PositionProfileRecord&) noexcept;
[[nodiscard]] bool decode(const PositionProfileRecord&,
                          gameplay::entity_position_profiles::Row&) noexcept;

[[nodiscard]] bool encode(const content::Definition& value, NamedRecord& record) noexcept;
[[nodiscard]] bool decode(const NamedRecord& record, content::Definition& value) noexcept;

[[nodiscard]] bool encode(const items::Definition& value, ItemRecord& record) noexcept;
[[nodiscard]] bool decode(const ItemRecord& record, items::Definition& value) noexcept;

[[nodiscard]] bool encode(const collectibles::Definition& value,
                          CollectibleRecord& record) noexcept;
[[nodiscard]] bool decode(const CollectibleRecord& record,
                          collectibles::Definition& value) noexcept;

[[nodiscard]] bool encode(const material_requirements::Definition& value,
                          MaterialRequirementSetRecord& record) noexcept;
[[nodiscard]] bool decode(const MaterialRequirementSetRecord& record,
                          material_requirements::Definition& value) noexcept;

[[nodiscard]] bool encode(const items::details::Definition& value,
                          ItemDetailRecord& record) noexcept;
[[nodiscard]] bool decode(const ItemDetailRecord& record,
                          items::details::Definition& value) noexcept;

[[nodiscard]] bool encode(const items::socket_plugs::Rule& value,
                          SocketPlugRuleRecord& record) noexcept;
[[nodiscard]] bool decode(const SocketPlugRuleRecord& record,
                          items::socket_plugs::Rule& value) noexcept;

[[nodiscard]] bool encode(const items::socket_plugs::Pool& value,
                          SocketPlugPoolRecord& record) noexcept;
[[nodiscard]] bool decode(const SocketPlugPoolRecord& record,
                          items::socket_plugs::Pool& value) noexcept;

[[nodiscard]] bool encode(items::socket_plugs::Member value,
                          SocketPlugMemberRecord& record) noexcept;
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

[[nodiscard]] bool encode(const inventory::buckets::Descriptor& value,
                          InventoryBucketRecord& record) noexcept;
[[nodiscard]] bool decode(const InventoryBucketRecord& record,
                          inventory::buckets::Descriptor& value) noexcept;

[[nodiscard]] bool encode(const socket_entry_lists::Definition& value,
                          SocketEntryListRecord& record) noexcept;
[[nodiscard]] bool decode(const SocketEntryListRecord& record,
                          socket_entry_lists::Definition& value) noexcept;

[[nodiscard]] bool encode(const socket_entry_lists::EntryTable& value,
                          SocketEntryTableRecord& record) noexcept;
[[nodiscard]] bool decode(const SocketEntryTableRecord& record,
                          socket_entry_lists::EntryTable& value) noexcept;

[[nodiscard]] bool encode(const abilities::Definition& value, AbilityBucketRecord& record) noexcept;
[[nodiscard]] bool decode(const AbilityBucketRecord& record, abilities::Definition& value) noexcept;

[[nodiscard]] bool encode(const progressions::Definition& value,
                          ProgressionRecord& record) noexcept;
[[nodiscard]] bool decode(const ProgressionRecord& record,
                          progressions::Definition& value) noexcept;

[[nodiscard]] bool encode(const progressions::Step& value, ProgressionStepRecord& record) noexcept;
[[nodiscard]] bool decode(const ProgressionStepRecord& record, progressions::Step& value) noexcept;

[[nodiscard]] bool encode(const season_pass::Reward& value,
                          SeasonPassRewardRecord& record) noexcept;
[[nodiscard]] bool decode(const SeasonPassRewardRecord& record,
                          season_pass::Reward& value) noexcept;

[[nodiscard]] bool encode(const bounties::Definition& value, BountyRecord& record) noexcept;
[[nodiscard]] bool decode(const BountyRecord& record, bounties::Definition& value) noexcept;

[[nodiscard]] bool encode(const build_data::records::Objective& value,
                          RecordObjectiveRecord& record) noexcept;
[[nodiscard]] bool decode(const RecordObjectiveRecord& record,
                          build_data::records::Objective& value) noexcept;

[[nodiscard]] bool encode(const build_data::records::Interval& value,
                          RecordIntervalRecord& record) noexcept;
[[nodiscard]] bool decode(const RecordIntervalRecord& record,
                          build_data::records::Interval& value) noexcept;

[[nodiscard]] bool encode(const build_data::records::Reward& value,
                          RecordRewardRecord& record) noexcept;
[[nodiscard]] bool decode(const RecordRewardRecord& record,
                          build_data::records::Reward& value) noexcept;

[[nodiscard]] bool encode(const build_data::records::Definition& value,
                          RecordDefinitionRecord& record) noexcept;
[[nodiscard]] bool decode(const RecordDefinitionRecord& record,
                          build_data::records::Definition& value) noexcept;

[[nodiscard]] bool encode(const nodes::Definition& value, NodeDefinitionRecord& record) noexcept;
[[nodiscard]] bool decode(const NodeDefinitionRecord& record, nodes::Definition& value) noexcept;

[[nodiscard]] bool encode(const sobjects::Definition& value,
                          SObjectDefinitionRecord& record) noexcept;
[[nodiscard]] bool decode(const SObjectDefinitionRecord& record,
                          sobjects::Definition& value) noexcept;

[[nodiscard]] bool encode(const scenarios::Definition& value, ScenarioRecord& record) noexcept;
[[nodiscard]] bool decode(const ScenarioRecord& record, scenarios::Definition& value) noexcept;

[[nodiscard]] bool encode(const scenarios::RosterGroup& value, RosterGroupRecord& record) noexcept;
[[nodiscard]] bool decode(const RosterGroupRecord& record, scenarios::RosterGroup& value) noexcept;

[[nodiscard]] bool encode(const hash_names::Name& value, HashNameRecord& record) noexcept;
[[nodiscard]] bool decode(const HashNameRecord& record, hash_names::Name& value) noexcept;

[[nodiscard]] bool encode(const spawn_sets::Stem& value, SpawnStemRecord& record) noexcept;
[[nodiscard]] bool decode(const SpawnStemRecord& record, spawn_sets::Stem& value) noexcept;

[[nodiscard]] bool encode(const spawn_sets::NameHash& value, SpawnNameHashRecord& record) noexcept;
[[nodiscard]] bool decode(const SpawnNameHashRecord& record, spawn_sets::NameHash& value) noexcept;

[[nodiscard]] bool encode(const spawn_sets::Point& value, SpawnPointRecord& record) noexcept;
[[nodiscard]] bool decode(const SpawnPointRecord& record, spawn_sets::Point& value) noexcept;

[[nodiscard]] bool encode(const vendors::IndexEntry& value, VendorIndexRecord& record) noexcept;
[[nodiscard]] bool decode(const VendorIndexRecord& record, vendors::IndexEntry& value) noexcept;

[[nodiscard]] bool encode(const vendors::Definition& value,
                          VendorDefinitionRecord& record) noexcept;
[[nodiscard]] bool decode(const VendorDefinitionRecord& record,
                          vendors::Definition& value) noexcept;

[[nodiscard]] bool encode(const vendors::SaleRow& value, VendorSaleRowRecord& record) noexcept;
[[nodiscard]] bool decode(const VendorSaleRowRecord& record, vendors::SaleRow& value) noexcept;

[[nodiscard]] bool encode(const vendors::InstalledRow& value,
                          VendorInstalledRowRecord& record) noexcept;
[[nodiscard]] bool decode(const VendorInstalledRowRecord& record,
                          vendors::InstalledRow& value) noexcept;

[[nodiscard]] bool encode(const rewards::Pool& value, RewardPoolRecord& record) noexcept;
[[nodiscard]] bool decode(const RewardPoolRecord& record, rewards::Pool& value) noexcept;
[[nodiscard]] bool encode(const rewards::Entry& value, RewardEntryRecord& record) noexcept;
[[nodiscard]] bool decode(const RewardEntryRecord& record, rewards::Entry& value) noexcept;
[[nodiscard]] bool encode(const rewards::Item& value, RewardItemRecord& record) noexcept;
[[nodiscard]] bool decode(const RewardItemRecord& record, rewards::Item& value) noexcept;
[[nodiscard]] bool encode(const rewards::Instruction& value,
                          RewardInstructionRecord& record) noexcept;
[[nodiscard]] bool decode(const RewardInstructionRecord& record,
                          rewards::Instruction& value) noexcept;
[[nodiscard]] bool encode(const rewards::Modifier& value, RewardModifierRecord& record) noexcept;
[[nodiscard]] bool decode(const RewardModifierRecord& record, rewards::Modifier& value) noexcept;
[[nodiscard]] bool encode(const rewards::SocketOverride& value,
                          RewardSocketOverrideRecord& record) noexcept;
[[nodiscard]] bool decode(const RewardSocketOverrideRecord& record,
                          rewards::SocketOverride& value) noexcept;

} // namespace sunrise::state::build_data::cache::records
