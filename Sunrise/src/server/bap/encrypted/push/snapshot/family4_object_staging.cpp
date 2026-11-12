#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/family4/instance/instance_encoder.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

namespace family4_datagen = middleware::datagen::family4;

/** Logs which step of preparation failed. @return Always false. */
bool report_failure(const char* step) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=family4 stage=prepare result=fail step=%s", step);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/** Reads the account-object fields State holds outside the account copy. */
bool read_account_state_fields(const state::AccountState& account,
                               family4_datagen::account::StateFields& fields) noexcept {
    state::synthesizer_tier_values(account, fields.characterValueRow, fields.characterValues);
    return state::read_store_wallet_sync(fields.storeSyncRevision);
}

/** Compresses one raw object and advances the shared sealed-buffer extent. */
bool append_object(Scratch& scratch,
                   std::span<const std::byte> encoded,
                   std::uint32_t definitionId,
                   std::uint64_t version,
                   middleware::queuez::Object& object,
                   std::size_t& compressedExtent) noexcept {
    std::size_t compressedSize = 0;
    if (!compress_object(
            scratch, encoded, definitionId, version, compressedExtent, object, compressedSize)) {
        return false;
    }
    compressedExtent += compressedSize;
    return true;
}

/** Encodes and appends one character's row-sorted item instances after any already staged. */
bool append_items(Scratch& scratch,
                  std::span<std::byte> rawStorage,
                  std::uint32_t itemInstanceObjectId,
                  const family4_datagen::loadout::ResolvedInstances& instances,
                  std::size_t baseIndex,
                  Prepared& staged,
                  std::size_t& itemCursor,
                  std::size_t& compressedExtent) noexcept {
    if (instances.itemCount > instances.items.size()
        || family4_datagen::instance::layout::kObjectSize > rawStorage.size()
        || baseIndex + itemCursor + instances.itemCount > staged.objects.size()) {
        return false;
    }

    const auto encoded = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
    for (std::size_t itemIndex = 0; itemIndex < instances.itemCount; ++itemIndex) {
        const family4_datagen::instance::ResolvedInstance& instance =
            instances.items[itemIndex].instance;
        const std::size_t objectIndex = baseIndex + itemCursor;
        if (!family4_datagen::instance::encode(instance, encoded)) {
            return false;
        }
        if (!append_object(scratch,
                           encoded,
                           itemInstanceObjectId,
                           instance.instanceSoid,
                           staged.objects[objectIndex],
                           compressedExtent)) {
            return false;
        }
        ++itemCursor;
    }
    return true;
}

/** Resolves one source-backed profile stack into the shared Family-4 item-instance schema. */
bool resolve_profile_item_instance(const state::account::inventory::ProfileItem& profileItem,
                                   family4_datagen::instance::ResolvedInstance& output) noexcept {
    output = {};
    const std::size_t itemDefinitionCount = state::build_data::item_definition_count();
    const std::size_t socketEntryListCount = state::build_data::socket_entry_list_count();
    state::build_data::items::Definition item{};
    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    state::build_data::socket_entry_lists::Definition socketList{};
    if (profileItem.instanceSoid == 0 || profileItem.quantity <= 0 || profileItem.mutationSerial < 0
        || itemDefinitionCount == 0
        || itemDefinitionCount > state::build_data::items::kDefinitionCapacity
        || socketEntryListCount == 0
        || socketEntryListCount > state::build_data::socket_entry_lists::kDefinitionCapacity
        || !state::build_data::find_item_definition_hash(profileItem.definitionHash, item)
        || item.definitionHash != profileItem.definitionHash
        || !state::build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex
        || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
        || detail.equipmentSlot.has_value()
        || detail.instancedDefinitionState
               != state::build_data::items::details::InstancedDefinitionState::stackable
        || detail.ordinarySocketState
               != state::build_data::items::details::OrdinarySocketState::absent
        || detail.ordinarySocketCount != 0
        || !state::build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
        || bucket.arraySelector != state::build_data::inventory::buckets::ArraySelector::profile
        || !state::build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
        || !state::build_data::find_socket_entry_list(detail.socketEntryListIndex, socketList)
        || socketList.definitionIndex != detail.socketEntryListIndex || socketList.entryCount != 0
        || static_cast<std::size_t>(item.definitionIndex) >= itemDefinitionCount
        || static_cast<std::size_t>(socketList.definitionIndex) >= socketEntryListCount) {
        return false;
    }

    family4_datagen::instance::ResolvedInstance candidate{};
    candidate.instanceSoid = profileItem.instanceSoid;
    candidate.bounds.itemDefinitionCount = static_cast<std::uint32_t>(itemDefinitionCount);
    candidate.bounds.socketEntryListCount = static_cast<std::uint32_t>(socketEntryListCount);
    candidate.baseDefinitionIndex = item.definitionIndex;
    candidate.level = family4_datagen::instance::layout::kMinimumItemLevel;
    candidate.curveSelector = family4_datagen::instance::layout::kInitialLevelCurveX;
    candidate.capSelector = family4_datagen::instance::layout::kInitialLevelCapRow;
    candidate.socketEntryListIndex = socketList.definitionIndex;
    candidate.socketEntryCount = 0;
    candidate.socketEntryContentsResolved = true;
    candidate.ordinarySockets.state = family4_datagen::instance::OrdinarySocketBlockState::absent;
    if (state::build_data::item_definition_count() != itemDefinitionCount
        || state::build_data::socket_entry_list_count() != socketEntryListCount) {
        return false;
    }
    output = candidate;
    return true;
}

/** Appends every source-backed profile item after all character-owned item residents. */
bool append_profile_items(Scratch& scratch,
                          std::span<std::byte> rawStorage,
                          std::uint32_t itemInstanceObjectId,
                          const state::AccountState& account,
                          std::size_t baseIndex,
                          Prepared& staged,
                          std::size_t& itemCursor,
                          std::size_t& compressedExtent) noexcept {
    if (account.profileItemCount > account.profileItems.size()
        || family4_datagen::instance::layout::kObjectSize > rawStorage.size()) {
        return false;
    }
    const auto encoded = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
    std::size_t appendedCount = 0;
    for (std::size_t profileIndex = 0; profileIndex < account.profileItemCount; ++profileIndex) {
        const state::account::inventory::ProfileItem& item = account.profileItems[profileIndex];
        if (item.instanceSoid == 0) {
            continue;
        }
        if (appendedCount >= state::account::inventory::kProfileActionSourceCapacity
            || baseIndex + itemCursor >= staged.objects.size()) {
            return false;
        }
        family4_datagen::instance::ResolvedInstance instance{};
        if (!resolve_profile_item_instance(item, instance)
            || !family4_datagen::instance::encode(instance, encoded)
            || !append_object(scratch,
                              encoded,
                              itemInstanceObjectId,
                              instance.instanceSoid,
                              staged.objects[baseIndex + itemCursor],
                              compressedExtent)) {
            return false;
        }
        ++itemCursor;
        ++appendedCount;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
