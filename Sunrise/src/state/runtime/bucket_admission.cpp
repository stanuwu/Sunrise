#include "bucket_admission.h"

#include <algorithm>
#include <bitset>
#include <limits>
#include <span>

#include "../../middleware/datagen/family4/account/layout.h"
#include "../../middleware/datagen/family4/character/layout.h"
#include "../account/inventory/occupied_bucket.h"
#include "../build_data/runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail {
namespace authored_inventory = account::inventory;
namespace inventory_buckets = build_data::inventory::buckets;
namespace item_details = build_data::items::details;

/** Saved profile rows record no placement, so only a character bucket can hold a recovered row. */
bool find_recovery_bucket(const build_data::items::Definition& item,
                          inventory_buckets::Descriptor& recovery) noexcept {
    return authored_inventory::find_occupied_bucket(
               item, authored_inventory::ItemPlacement::recovery, recovery)
           && recovery.arraySelector == inventory_buckets::ArraySelector::character;
}

namespace {

/** Counts occupied rows and remembers the oldest replaceable row in their owning array. */
struct BucketOccupancy {
    std::size_t count{};
    std::size_t oldest{(std::numeric_limits<std::size_t>::max)()};
    std::int32_t serial{(std::numeric_limits<std::int32_t>::max)()};

    /**
     * Counts one replaceable row and keeps the lowest mutation serial as the oldest. Rows sharing
     * a serial keep the lowest index, whatever order they are counted in.
     */
    void include(std::size_t index, std::int32_t mutationSerial) noexcept {
        ++count;
        if (oldest == (std::numeric_limits<std::size_t>::max)() || mutationSerial < serial
            || (mutationSerial == serial && index < oldest)) {
            oldest = index;
            serial = mutationSerial;
        }
    }
};

/**
 * Buckets an eviction chain is still freeing a row in. The client inserts an evicted row's copy
 * before clearing its slot, so a chain leading back into a bucket on it never finishes there. Here
 * the victim leaves first, and a transfer back would take the row freed for the arrival, so that
 * victim is discarded instead.
 */
using EvictionChain = std::bitset<inventory_buckets::kDescriptorCapacity>;

/** @return True when each unit of the item is its own instance rather than part of a stack. */
[[nodiscard]] bool instanced(const item_details::Definition& detail) noexcept {
    return detail.instancedDefinitionState == item_details::InstancedDefinitionState::instanced;
}

/** @return True for the two arrays that share the character object's change ring. */
[[nodiscard]] bool character_ring(GrantedRowArray array) noexcept {
    return array != GrantedRowArray::profile;
}

/**
 * Records one created, grown or moved row. Each change list is a ring the client overwrites
 * oldest first, so once a batch fills one ring a new row there displaces that ring's oldest.
 */
void record(PlacedRows& rows,
            GrantedRowArray array,
            std::int32_t mutationSerial,
            bool newRow) noexcept {
    const std::size_t capacity =
        character_ring(array)
            ? middleware::datagen::family4::character::layout::kInventoryChangeRecordCapacity
            : middleware::datagen::family4::account::layout::kProfileInventoryChangeRecordCapacity;
    const auto held = std::span(rows.values).first(rows.count);
    const auto sameRing = [&](const GrantedRow& row) noexcept {
        return character_ring(row.array) == character_ring(array);
    };
    if (static_cast<std::size_t>(std::count_if(held.begin(), held.end(), sameRing)) == capacity) {
        const auto oldest = std::find_if(held.begin(), held.end(), sameRing);
        std::move(oldest + 1, held.end(), oldest);
        --rows.count;
    }
    rows.values[rows.count++] = {array, mutationSerial, newRow};
}

// A batch holds a full ring of each kind at once.
static_assert(
    kRecordRewardGrantCapacity
    >= middleware::datagen::family4::character::layout::kInventoryChangeRecordCapacity
           + middleware::datagen::family4::account::layout::kProfileInventoryChangeRecordCapacity);

/** @return Room the same item's stacks in this bucket still have. */
[[nodiscard]] std::int64_t stack_headroom(const AccountState& account,
                                          std::size_t characterIndex,
                                          const inventory_buckets::Descriptor& bucket,
                                          const build_data::items::Definition& item,
                                          std::int32_t maxStackSize) noexcept {
    std::int64_t headroom = 0;
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            const auto& row = account.profileItems[index];
            if (row.definitionHash == item.definitionHash && row.quantity < maxStackSize) {
                headroom += maxStackSize - row.quantity;
            }
        }
        return headroom;
    }
    authored_inventory::ItemPlacement placement{};
    if (!authored_inventory::placement_in(item, bucket.bucketId, placement)) {
        return headroom;
    }
    const auto& stacks = account.characters[characterIndex].stacks;
    for (std::size_t index = 0; index < stacks.count; ++index) {
        const auto& row = stacks.values[index];
        if (row.definitionHash == item.definitionHash && row.placement == placement
            && row.quantity < maxStackSize) {
            headroom += maxStackSize - row.quantity;
        }
    }
    return headroom;
}

/** One saved row as the fit rules see it: its item, quantity, age and the bucket it occupies. */
struct HeldRow {
    build_data::items::Definition item{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    /** A profile index, or an index in the character row space kStackRowBase divides. */
    std::size_t row{};
    std::uint8_t bucketId{};
};

/**
 * Visits every row of the array a bucket belongs to, equipped rows first, in saved order. The
 * client walks its slots in order instead; that order only picks among rows sharing one label, and
 * the label rule never lets a second such row form.
 * @param visit Returns false to stop the walk.
 * @return False when a saved row names no installed item.
 */
template <typename Visit>
[[nodiscard]] bool visit_array(const AccountState& account,
                               std::size_t characterIndex,
                               const inventory_buckets::Descriptor& bucket,
                               Visit&& visit) noexcept {
    HeldRow held{};
    const auto resolve = [&held](std::uint32_t definitionHash,
                                 std::int32_t quantity,
                                 std::int32_t mutationSerial,
                                 authored_inventory::ItemPlacement placement,
                                 std::size_t row) noexcept {
        if (!build_data::find_item_definition_hash(definitionHash, held.item)) {
            return false;
        }
        held.quantity = quantity;
        held.mutationSerial = mutationSerial;
        held.row = row;
        held.bucketId = authored_inventory::occupied_bucket_id(held.item, placement);
        return true;
    };
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            const auto& item = account.profileItems[index];
            if (!resolve(item.definitionHash,
                         item.quantity,
                         item.mutationSerial,
                         authored_inventory::ItemPlacement::authored,
                         index)) {
                return false;
            }
            if (!visit(held)) {
                return true;
            }
        }
        return true;
    }
    const CharacterState& character = account.characters[characterIndex];
    for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
        const auto& equipped = character.equipment.slots[slot];
        if (equipped
            && !resolve(equipped->definitionHash,
                        1,
                        equipped->mutationSerial,
                        equipped->placement,
                        kEquippedRowBase + slot)) {
            return false;
        }
        if (equipped && !visit(held)) {
            return true;
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        const auto& item = character.inventory.values[index];
        if (!resolve(
                item.definitionHash, item.quantity, item.mutationSerial, item.placement, index)) {
            return false;
        }
        if (!visit(held)) {
            return true;
        }
    }
    for (std::size_t index = 0; index < character.stacks.count; ++index) {
        const auto& item = character.stacks.values[index];
        if (!resolve(item.definitionHash,
                     item.quantity,
                     item.mutationSerial,
                     item.placement,
                     kStackRowBase + index)) {
            return false;
        }
        if (!visit(held)) {
            return true;
        }
    }
    return true;
}

/**
 * Counts one bucket's rows in whichever saved array it belongs to and picks its oldest. As in the
 * client, an equipped row is an ordinary occupied row of its bucket.
 */
[[nodiscard]] bool bucket_occupancy(const AccountState& account,
                                    std::size_t characterIndex,
                                    const inventory_buckets::Descriptor& bucket,
                                    BucketOccupancy& occupancy) noexcept {
    occupancy = {};
    if (bucket.arraySelector == inventory_buckets::ArraySelector::character) {
        const CharacterState& character = account.characters[characterIndex];
        if (character.inventory.count > character.inventory.values.size()
            || character.stacks.count > character.stacks.values.size()) {
            return false;
        }
    } else if (bucket.arraySelector != inventory_buckets::ArraySelector::profile) {
        return false;
    }
    return visit_array(account, characterIndex, bucket, [&](const HeldRow& held) noexcept {
        if (held.bucketId == bucket.bucketId) {
            occupancy.include(held.row, held.mutationSerial);
        }
        return true;
    });
}

/**
 * A labelled item never shares its label with another row. The first row in the destination array
 * carrying the label decides: the same stackable item there takes what its stack has room for, any
 * other row admits nothing. With no such row, one new row is admitted if the bucket can take it.
 */
[[nodiscard]] bool label_fit(const AccountState& account,
                             std::size_t characterIndex,
                             const inventory_buckets::Descriptor& bucket,
                             const build_data::items::Definition& item,
                             const item_details::Definition& detail,
                             std::int32_t quantity,
                             bool roomForRow,
                             std::int32_t& admitted) noexcept {
    admitted = 0;
    bool decided = false;
    if (!visit_array(account, characterIndex, bucket, [&](const HeldRow& row) noexcept {
            if (row.item.stackUniqueLabel != item.stackUniqueLabel) {
                return true;
            }
            decided = true;
            if (row.item.definitionHash == item.definitionHash && !instanced(detail)
                && row.bucketId == bucket.bucketId && row.quantity < detail.maxStackSize) {
                admitted = (std::min)(quantity, detail.maxStackSize - row.quantity);
            }
            return false;
        })) {
        return false;
    }
    if (!decided && roomForRow) {
        admitted = (std::min)(quantity, instanced(detail) ? 1 : detail.maxStackSize);
    }
    return true;
}

/**
 * How much of an arrival one bucket admits: everything for a FIFO bucket, otherwise its free rows
 * at a whole stack each plus the room left on the item's own stacks there.
 * @param scanLabel False for a claim within one array, which the whole-row rule covers instead.
 */
[[nodiscard]] bool admitted_quantity(const AccountState& account,
                                     std::size_t characterIndex,
                                     const inventory_buckets::Descriptor& bucket,
                                     const build_data::items::Definition& item,
                                     const item_details::Definition& detail,
                                     std::int32_t quantity,
                                     bool scanLabel,
                                     std::int32_t& admitted) noexcept {
    admitted = 0;
    BucketOccupancy occupancy;
    if (!bucket_occupancy(account, characterIndex, bucket, occupancy)) {
        return false;
    }
    const std::size_t freeRows =
        occupancy.count < bucket.slotCount ? bucket.slotCount - occupancy.count : 0;
    const bool fifo = (bucket.policyFlags & inventory_buckets::kFifo) != 0;
    if (scanLabel && item.stackUniqueLabel != build_data::items::kNoStackUniqueLabel) {
        return label_fit(account,
                         characterIndex,
                         bucket,
                         item,
                         detail,
                         quantity,
                         fifo || freeRows != 0,
                         admitted);
    }
    if (fifo) {
        admitted = quantity;
        return true;
    }
    if (instanced(detail)) {
        admitted = freeRows != 0 ? quantity : 0;
        return true;
    }
    const std::int64_t room =
        static_cast<std::int64_t>(freeRows) * detail.maxStackSize
        + stack_headroom(account, characterIndex, bucket, item, detail.maxStackSize);
    admitted = static_cast<std::int32_t>((std::min)(static_cast<std::int64_t>(quantity), room));
    return true;
}

[[nodiscard]] bool insert_stack(AccountState& working,
                                std::size_t characterIndex,
                                const inventory_buckets::Descriptor& bucket,
                                const build_data::items::Definition& item,
                                const item_details::Definition& detail,
                                std::int32_t quantity,
                                EvictionChain& chain,
                                PlacedRows& rows,
                                const char*& reason) noexcept;

[[nodiscard]] bool insert_instance(AccountState& working,
                                   std::size_t characterIndex,
                                   const inventory_buckets::Descriptor& bucket,
                                   const build_data::items::Definition& definition,
                                   authored_inventory::Item item,
                                   EvictionChain& chain,
                                   PlacedRows& rows,
                                   const char*& reason) noexcept;

/**
 * The client's label check on a victim, which still occupies its row: the first row in the array
 * carrying the label must be the same stackable item, with room for the victim's quantity too.
 */
[[nodiscard]] bool victim_label_fits(const AccountState& account,
                                     std::size_t characterIndex,
                                     const inventory_buckets::Descriptor& bucket,
                                     const build_data::items::Definition& victim,
                                     const item_details::Definition& detail,
                                     std::int32_t quantity,
                                     bool& fits) noexcept {
    fits = victim.stackUniqueLabel == build_data::items::kNoStackUniqueLabel;
    if (fits) {
        return true;
    }
    return visit_array(account, characterIndex, bucket, [&](const HeldRow& row) noexcept {
        if (row.item.stackUniqueLabel != victim.stackUniqueLabel) {
            return true;
        }
        fits = row.item.definitionHash == victim.definitionHash && !instanced(detail)
               && detail.maxStackSize > 1
               && static_cast<std::int64_t>(row.quantity) + quantity <= detail.maxStackSize;
        return false;
    });
}

/**
 * Whether a full FIFO bucket's victim moves to its recovery bucket, in the client's order: the
 * bucket's no-transfer bit, a recovery bucket other than this one in the same array, the restricted
 * list, the label, then room when the recovery bucket is not FIFO. No kind Sunrise publishes is
 * one the restricted list lets through.
 * @param bucket A character bucket; find_recovery_bucket names buckets of that array only.
 */
[[nodiscard]] bool transfers_victim(const AccountState& account,
                                    std::size_t characterIndex,
                                    const inventory_buckets::Descriptor& bucket,
                                    const build_data::items::Definition& victim,
                                    const item_details::Definition& detail,
                                    std::int32_t quantity,
                                    const EvictionChain& chain,
                                    inventory_buckets::Descriptor& recovery,
                                    bool& transfers) noexcept {
    transfers = false;
    bool labelFits = false;
    if ((bucket.policyFlags & inventory_buckets::kNoTransferOnEviction) != 0
        || victim.recoveryBucketId == bucket.bucketId || !find_recovery_bucket(victim, recovery)
        || chain.test(recovery.bucketId) || victim.grantRestricted) {
        return true;
    }
    if (!victim_label_fits(account, characterIndex, bucket, victim, detail, quantity, labelFits)) {
        return false;
    }
    if (!labelFits) {
        return true;
    }
    if ((recovery.policyFlags & inventory_buckets::kFifo) != 0) {
        transfers = true;
        return true;
    }
    std::int32_t admitted = 0;
    if (!admitted_quantity(
            account, characterIndex, recovery, victim, detail, quantity, false, admitted)) {
        return false;
    }
    transfers = admitted >= quantity;
    return true;
}

/**
 * Evicts the oldest row of a full character FIFO bucket, moving it when allowed.
 * @param oldest The bucket's oldest row as bucket_occupancy found it in this account.
 */
[[nodiscard]] bool evict_character_row(AccountState& working,
                                       std::size_t characterIndex,
                                       const inventory_buckets::Descriptor& bucket,
                                       std::size_t oldest,
                                       EvictionChain& chain,
                                       PlacedRows& rows,
                                       const char*& reason) noexcept {
    CharacterState& character = working.characters[characterIndex];
    reason = "fifo_victim";
    if (oldest >= kCharacterRowCount) {
        return false;
    }
    const bool victimStack = oldest >= kStackRowBase && oldest < kEquippedRowBase;
    authored_inventory::Item instance{};
    authored_inventory::CharacterStack stack{};
    if (victimStack) {
        stack = character.stacks.values[oldest - kStackRowBase];
    } else if (oldest < kStackRowBase) {
        instance = character.inventory.values[oldest];
    } else {
        instance = *character.equipment.slots[oldest - kEquippedRowBase];
    }
    build_data::items::Definition victim{};
    item_details::Definition victimDetail{};
    inventory_buckets::Descriptor recovery{};
    bool transfers = false;
    if (!build_data::find_item_definition_hash(
            victimStack ? stack.definitionHash : instance.definitionHash, victim)
        || !build_data::find_configured_item_detail(victim.definitionIndex, victimDetail)
        || !transfers_victim(working,
                             characterIndex,
                             bucket,
                             victim,
                             victimDetail,
                             victimStack ? stack.quantity : instance.quantity,
                             chain,
                             recovery,
                             transfers)
        || !erase_character_row(character, oldest)) {
        return false;
    }
    if (!transfers) {
        return true;
    }
    if (!victimStack) {
        return insert_instance(
            working, characterIndex, recovery, victim, instance, chain, rows, reason);
    }
    return insert_stack(working,
                        characterIndex,
                        recovery,
                        victim,
                        victimDetail,
                        stack.quantity,
                        chain,
                        rows,
                        reason);
}

/**
 * Evicts the oldest row of a full profile FIFO bucket. The client moves a victim only within its
 * array, and saved profile rows record no placement, so a profile victim is discarded.
 * @param oldest The bucket's oldest row as bucket_occupancy found it in this account.
 */
[[nodiscard]] bool evict_profile_row(AccountState& working,
                                     std::size_t oldest,
                                     const char*& reason) noexcept {
    reason = "fifo_victim";
    if (oldest >= working.profileItemCount) {
        return false;
    }
    // An action-source row is a published resident; evicting one is unsupported, so this refuses.
    reason = "fifo_resident";
    if (working.profileItems[oldest].instanceSoid != 0) {
        return false;
    }
    reason = "fifo_victim";
    return erase_profile_row(working, oldest);
}

/** Makes one free row in a bucket, evicting under FIFO policy; an ordinary bucket refuses. */
[[nodiscard]] bool free_row(AccountState& working,
                            std::size_t characterIndex,
                            const inventory_buckets::Descriptor& bucket,
                            EvictionChain& chain,
                            PlacedRows& rows,
                            const char*& reason) noexcept {
    BucketOccupancy occupancy;
    reason = "bucket_rows";
    if (!bucket_occupancy(working, characterIndex, bucket, occupancy)) {
        return false;
    }
    if (occupancy.count < bucket.slotCount) {
        return true;
    }
    reason = "bucket_full";
    if ((bucket.policyFlags & inventory_buckets::kFifo) == 0) {
        return false;
    }
    chain.set(bucket.bucketId);
    const bool evicted =
        bucket.arraySelector == inventory_buckets::ArraySelector::profile
            ? evict_profile_row(working, occupancy.oldest, reason)
            : evict_character_row(
                  working, characterIndex, bucket, occupancy.oldest, chain, rows, reason);
    chain.reset(bucket.bucketId);
    return evicted;
}

/**
 * The client's merge into one same-item row: it takes what fits, and gains a fresh serial even
 * when nothing fits, so a full stack the arrival touched is no longer the oldest row.
 * @return The quantity the row took.
 */
template <typename Row>
[[nodiscard]] std::int32_t merge_into(Row& row,
                                      std::int32_t serial,
                                      std::int32_t quantity,
                                      std::int32_t maxStackSize) noexcept {
    const std::int32_t added = (std::min)(quantity, (std::max)(maxStackSize - row.quantity, 0));
    row.quantity += added;
    row.mutationSerial = serial;
    return added;
}

/** Merges into the item's own profile stacks, then adds whole new ones in its authored bucket. */
[[nodiscard]] bool insert_profile_stack(AccountState& working,
                                        std::size_t characterIndex,
                                        const inventory_buckets::Descriptor& bucket,
                                        const build_data::items::Definition& item,
                                        const item_details::Definition& detail,
                                        std::int32_t quantity,
                                        EvictionChain& chain,
                                        PlacedRows& rows,
                                        const char*& reason) noexcept {
    // A saved profile row records no placement, so it only ever sits in its authored bucket.
    if (bucket.bucketId != item.bucketId) {
        return false;
    }
    const bool labelled = item.stackUniqueLabel != build_data::items::kNoStackUniqueLabel;
    for (std::size_t index = 0; index < working.profileItemCount; ++index) {
        auto& row = working.profileItems[index];
        std::int32_t serial = 0;
        if (row.definitionHash != item.definitionHash) {
            continue;
        }
        reason = "serial_exhausted";
        if (!next_profile_serial(working, serial)) {
            return false;
        }
        const std::int32_t added = merge_into(row, serial, quantity, detail.maxStackSize);
        if (added != 0) {
            record(rows, GrantedRowArray::profile, serial, false);
        }
        quantity -= added;
        if (labelled) {
            break;
        }
    }
    while (quantity > 0) {
        std::int32_t serial = 0;
        std::uint64_t soid = 0;
        if (!free_row(working, characterIndex, bucket, chain, rows, reason)) {
            return false;
        }
        reason = "profile_capacity";
        if (working.profileItemCount >= working.profileItems.size()) {
            return false;
        }
        reason = "serial_exhausted";
        if (!next_profile_serial(working, serial)) {
            return false;
        }
        reason = "instance_identity";
        if (build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
            && !next_profile_item_instance_soid(working, soid)) {
            return false;
        }
        record(rows, GrantedRowArray::profile, serial, true);
        const std::int32_t added = (std::min)(quantity, detail.maxStackSize);
        working.profileItems[working.profileItemCount++] = {
            soid, item.definitionHash, added, serial};
        quantity -= added;
    }
    return true;
}

/** Merges into the item's own stacks in a character bucket, then adds whole new ones. */
[[nodiscard]] bool insert_character_stack(AccountState& working,
                                          std::size_t characterIndex,
                                          const inventory_buckets::Descriptor& bucket,
                                          const build_data::items::Definition& item,
                                          const item_details::Definition& detail,
                                          std::int32_t quantity,
                                          EvictionChain& chain,
                                          PlacedRows& rows,
                                          const char*& reason) noexcept {
    authored_inventory::ItemPlacement placement{};
    if (!authored_inventory::placement_in(item, bucket.bucketId, placement)) {
        return false;
    }
    const bool labelled = item.stackUniqueLabel != build_data::items::kNoStackUniqueLabel;
    CharacterState& character = working.characters[characterIndex];
    for (std::size_t index = 0; index < character.stacks.count; ++index) {
        auto& row = character.stacks.values[index];
        std::int32_t serial = 0;
        if (row.definitionHash != item.definitionHash || row.placement != placement) {
            continue;
        }
        reason = "serial_exhausted";
        if (!next_character_serial(character, serial)) {
            return false;
        }
        const std::int32_t added = merge_into(row, serial, quantity, detail.maxStackSize);
        if (added != 0) {
            record(rows, GrantedRowArray::characterStack, serial, false);
        }
        quantity -= added;
        if (labelled) {
            break;
        }
    }
    while (quantity > 0) {
        std::int32_t serial = 0;
        if (!free_row(working, characterIndex, bucket, chain, rows, reason)) {
            return false;
        }
        reason = "character_stack_capacity";
        if (character.stacks.count >= character.stacks.values.size()) {
            return false;
        }
        reason = "serial_exhausted";
        if (!next_character_serial(character, serial)) {
            return false;
        }
        record(rows, GrantedRowArray::characterStack, serial, true);
        const std::int32_t added = (std::min)(quantity, detail.maxStackSize);
        character.stacks.values[character.stacks.count++] = {
            item.definitionHash, added, serial, placement};
        quantity -= added;
    }
    return true;
}

/** Places a stackable arrival in the array its bucket belongs to. */
bool insert_stack(AccountState& working,
                  std::size_t characterIndex,
                  const inventory_buckets::Descriptor& bucket,
                  const build_data::items::Definition& item,
                  const item_details::Definition& detail,
                  std::int32_t quantity,
                  EvictionChain& chain,
                  PlacedRows& rows,
                  const char*& reason) noexcept {
    reason = "stack_insert";
    if (quantity <= 0 || detail.maxStackSize <= 0 || instanced(detail)) {
        return false;
    }
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        return insert_profile_stack(
            working, characterIndex, bucket, item, detail, quantity, chain, rows, reason);
    }
    return bucket.arraySelector == inventory_buckets::ArraySelector::character
           && insert_character_stack(
               working, characterIndex, bucket, item, detail, quantity, chain, rows, reason);
}

/** Adds one instance, keeping its identity, in a free or freed row of a character bucket. */
bool insert_instance(AccountState& working,
                     std::size_t characterIndex,
                     const inventory_buckets::Descriptor& bucket,
                     const build_data::items::Definition& definition,
                     authored_inventory::Item item,
                     EvictionChain& chain,
                     PlacedRows& rows,
                     const char*& reason) noexcept {
    reason = "instance_insert";
    authored_inventory::ItemPlacement placement{};
    if (bucket.arraySelector != inventory_buckets::ArraySelector::character
        || !authored_inventory::placement_in(definition, bucket.bucketId, placement)
        || !free_row(working, characterIndex, bucket, chain, rows, reason)) {
        return false;
    }
    CharacterState& character = working.characters[characterIndex];
    std::int32_t serial = 0;
    reason = "instance_capacity";
    if (character.inventory.count >= character.inventory.values.size()) {
        return false;
    }
    reason = "serial_exhausted";
    if (!next_character_serial(character, serial)) {
        return false;
    }
    record(rows, GrantedRowArray::characterItem, serial, true);
    item.placement = placement;
    item.mutationSerial = serial;
    character.inventory.values[character.inventory.count++] = item;
    return true;
}

/** Inserts what one bucket admitted, allocating the identity of a new instance first. */
[[nodiscard]] bool insert_admitted(AccountState& working,
                                   std::size_t characterIndex,
                                   const inventory_buckets::Descriptor& bucket,
                                   const Arrival& arrival,
                                   std::int32_t quantity,
                                   std::uint64_t& nextInstanceSoid,
                                   EvictionChain& chain,
                                   PlacedRows& rows,
                                   const char*& reason) noexcept {
    if (!instanced(arrival.detail)) {
        return insert_stack(working,
                            characterIndex,
                            bucket,
                            arrival.item,
                            arrival.detail,
                            quantity,
                            chain,
                            rows,
                            reason);
    }
    reason = "instance_identity";
    authored_inventory::Item instance = arrival.instance;
    if (!next_item_instance_soid(working, instance.instanceSoid, nextInstanceSoid)
        || instance.instanceSoid == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    nextInstanceSoid = instance.instanceSoid + 1;
    instance.definitionHash = arrival.item.definitionHash;
    instance.quantity = 1;
    return insert_instance(
        working, characterIndex, bucket, arrival.item, instance, chain, rows, reason);
}

/** What a claim found in its source bucket. */
struct ClaimSource {
    /** The claimed instance's row, when the item is instanced. */
    std::size_t instanceRow{};
    /** The quantity every same-item stack in the source holds. */
    std::int64_t held{};
    /** The quantity of the first such stack. */
    std::int32_t firstRow{};
};

/** Finds the claimed instance, or the stacks of the item, among the rows in its recovery bucket. */
[[nodiscard]] bool find_claim_source(const CharacterState& character,
                                     const build_data::items::Definition& item,
                                     bool instance,
                                     std::uint64_t instanceSoid,
                                     std::int32_t quantity,
                                     ClaimSource& source) noexcept {
    const auto recovered = authored_inventory::ItemPlacement::recovery;
    source = {};
    if (instance) {
        source.instanceRow = character.inventory.count;
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            const auto& row = character.inventory.values[index];
            if (row.instanceSoid == instanceSoid && row.placement == recovered
                && row.definitionHash == item.definitionHash) {
                source.instanceRow = index;
            }
        }
        return instanceSoid != 0 && quantity == 1
               && source.instanceRow != character.inventory.count;
    }
    for (std::size_t index = 0; index < character.stacks.count; ++index) {
        const auto& row = character.stacks.values[index];
        if (row.definitionHash == item.definitionHash && row.placement == recovered) {
            source.firstRow = source.held == 0 ? row.quantity : source.firstRow;
            source.held += row.quantity;
        }
    }
    return instanceSoid == 0 && source.held >= quantity;
}

/** Debits the claim from its source with no change record, the stack quantity from the last row. */
[[nodiscard]] bool debit_claim_source(CharacterState& character,
                                      const build_data::items::Definition& item,
                                      bool instance,
                                      const ClaimSource& source,
                                      std::int32_t quantity,
                                      authored_inventory::Item& moved) noexcept {
    if (instance) {
        moved = character.inventory.values[source.instanceRow];
        return erase_character_row(character, source.instanceRow);
    }
    std::int32_t owed = quantity;
    for (std::size_t index = character.stacks.count; owed > 0 && index-- > 0;) {
        auto& row = character.stacks.values[index];
        if (row.definitionHash != item.definitionHash
            || row.placement != authored_inventory::ItemPlacement::recovery) {
            continue;
        }
        const std::int32_t taken = (std::min)(owed, row.quantity);
        row.quantity -= taken;
        owed -= taken;
        if (row.quantity == 0 && !erase_character_row(character, kStackRowBase + index)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** The serial is also the row's age, so the counter only moves forward. */
bool next_character_serial(CharacterState& character, std::int32_t& mutationSerial) noexcept {
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
    return true;
}

/** Profile rows take the serial after the greatest one the profile holds. */
bool next_profile_serial(const AccountState& account, std::int32_t& mutationSerial) noexcept {
    std::int32_t greatest = 0;
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        greatest = (std::max)(greatest, account.profileItems[index].mutationSerial);
    }
    if (greatest == (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    mutationSerial = greatest + 1;
    return true;
}

/** Removes one held instance, stack or equipped row named by its character row index. */
bool erase_character_row(CharacterState& character, std::size_t row) noexcept {
    const auto erase = [](auto& held, std::size_t index) noexcept {
        if (index >= held.count || held.count > held.values.size()) {
            return false;
        }
        std::move(held.values.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                  held.values.begin() + static_cast<std::ptrdiff_t>(held.count),
                  held.values.begin() + static_cast<std::ptrdiff_t>(index));
        held.values[--held.count] = {};
        return true;
    };
    if (row < kStackRowBase) {
        return erase(character.inventory, row);
    }
    if (row < kEquippedRowBase) {
        return erase(character.stacks, row - kStackRowBase);
    }
    if (row >= kCharacterRowCount || !character.equipment.slots[row - kEquippedRowBase]) {
        return false;
    }
    character.equipment.slots[row - kEquippedRowBase].reset();
    return true;
}

/** Later rows move up one, so the profile stays dense. */
bool erase_profile_row(AccountState& account, std::size_t index) noexcept {
    if (index >= account.profileItemCount
        || account.profileItemCount > account.profileItems.size()) {
        return false;
    }
    auto& items = account.profileItems;
    std::move(items.begin() + static_cast<std::ptrdiff_t>(index) + 1,
              items.begin() + static_cast<std::ptrdiff_t>(account.profileItemCount),
              items.begin() + static_cast<std::ptrdiff_t>(index));
    items[--account.profileItemCount] = {};
    return true;
}

/** Places what fits in the item's own bucket, then its recovery bucket, and drops the rest. */
RewardPreparation place_arrival(AccountState& working,
                                std::size_t characterIndex,
                                const Arrival& arrival,
                                std::uint64_t& nextInstanceSoid,
                                PlacedRows& rows,
                                std::int32_t& placed,
                                const char*& reason) noexcept {
    placed = 0;
    reason = "arrival";
    if (characterIndex >= working.characterCount) {
        return RewardPreparation::deferred;
    }
    // With the destination array checked by the caller, every later refusal depends on the rows
    // the account holds.
    inventory_buckets::Descriptor own{};
    if (arrival.quantity <= 0
        || (instanced(arrival.detail) ? arrival.quantity != 1 : arrival.detail.maxStackSize <= 0)
        || !build_data::find_inventory_bucket_descriptor(arrival.item.bucketId, own)) {
        return RewardPreparation::unresolvable;
    }
    // The client refuses a restricted item outright: neither its bucket nor its recovery gets any.
    if (arrival.item.grantRestricted) {
        reason = nullptr;
        return RewardPreparation::prepared;
    }
    std::int32_t remaining = arrival.quantity;
    EvictionChain chain;
    const auto admit = [&](const inventory_buckets::Descriptor& bucket) noexcept {
        std::int32_t admitted = 0;
        reason = "bucket_rows";
        if (!admitted_quantity(working,
                               characterIndex,
                               bucket,
                               arrival.item,
                               arrival.detail,
                               remaining,
                               true,
                               admitted)
            || (admitted != 0
                && !insert_admitted(working,
                                    characterIndex,
                                    bucket,
                                    arrival,
                                    admitted,
                                    nextInstanceSoid,
                                    chain,
                                    rows,
                                    reason))) {
            return false;
        }
        remaining -= admitted;
        return true;
    };
    inventory_buckets::Descriptor recovery{};
    if (!admit(own)
        || (remaining != 0 && find_recovery_bucket(arrival.item, recovery) && !admit(recovery))) {
        return RewardPreparation::deferred;
    }
    placed = arrival.quantity - remaining;
    reason = nullptr;
    return RewardPreparation::prepared;
}

/** Validates and moves one claim in the client's order: debit the source, then insert. */
bool place_claim(AccountState& working,
                 std::size_t characterIndex,
                 std::uint8_t sourceBucket,
                 const build_data::items::Definition& item,
                 const item_details::Definition& detail,
                 std::uint64_t instanceSoid,
                 std::int32_t quantity,
                 PlacedRows& rows,
                 const char*& reason) noexcept {
    reason = "claim_item";
    inventory_buckets::Descriptor destination{};
    inventory_buckets::Descriptor source{};
    if (characterIndex >= working.characterCount || quantity <= 0
        || item.recoveryBucketId != sourceBucket || !find_recovery_bucket(item, source)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, destination)
        || destination.bucketId == source.bucketId) {
        return false;
    }
    reason = "claim_restricted";
    if (item.grantRestricted) {
        return false;
    }
    CharacterState& character = working.characters[characterIndex];
    const bool instance = instanced(detail);
    ClaimSource claimed{};
    reason = "claim_presence";
    if (!find_claim_source(character, item, instance, instanceSoid, quantity, claimed)) {
        return false;
    }
    // A labelled stack claimed within one array moves its first row whole, or not at all.
    const bool sameArray = destination.arraySelector == source.arraySelector;
    reason = "claim_label";
    if (sameArray && !instance && item.stackUniqueLabel != build_data::items::kNoStackUniqueLabel
        && claimed.firstRow != quantity) {
        return false;
    }
    reason = "claim_capacity";
    std::int32_t admitted = 0;
    if (!admitted_quantity(
            working, characterIndex, destination, item, detail, quantity, !sameArray, admitted)
        || admitted < quantity) {
        return false;
    }
    authored_inventory::Item moved{};
    EvictionChain chain;
    reason = "claim_debit";
    if (!debit_claim_source(character, item, instance, claimed, quantity, moved)
        || (instance ? !insert_instance(
                           working, characterIndex, destination, item, moved, chain, rows, reason)
                     : !insert_stack(working,
                                     characterIndex,
                                     destination,
                                     item,
                                     detail,
                                     quantity,
                                     chain,
                                     rows,
                                     reason))) {
        return false;
    }
    reason = nullptr;
    return true;
}

/** Counts announced rows against each change list's capacity. */
bool change_rings_fit(std::span<const GrantedRow> rows) noexcept {
    std::size_t characterChanges = 0;
    std::size_t profileChanges = 0;
    for (const auto& row : rows) {
        if (row.array == GrantedRowArray::profile) {
            if (++profileChanges > middleware::datagen::family4::account::layout::
                    kProfileInventoryChangeRecordCapacity) {
                return false;
            }
        } else if (++characterChanges > middleware::datagen::family4::character::layout::
                       kInventoryChangeRecordCapacity) {
            return false;
        }
    }
    return true;
}

/** Looks a granted row up by its serial in the array it names. */
bool row_present(const AccountState& account,
                 std::size_t characterIndex,
                 const GrantedRow& row) noexcept {
    const auto matches = [&](const auto& held) noexcept {
        return held.mutationSerial == row.mutationSerial;
    };
    if (row.array == GrantedRowArray::profile) {
        const auto held = std::span(account.profileItems).first(account.profileItemCount);
        return std::any_of(held.begin(), held.end(), matches);
    }
    const CharacterState& character = account.characters[characterIndex];
    if (row.array == GrantedRowArray::characterItem) {
        const auto held = std::span(character.inventory.values).first(character.inventory.count);
        return std::any_of(held.begin(), held.end(), matches);
    }
    const auto held = std::span(character.stacks.values).first(character.stacks.count);
    return std::any_of(held.begin(), held.end(), matches);
}

/** Copies the before-image of the character and profile rows a batch can change. */
void record_grant_before(const AccountState& before,
                         std::size_t characterIndex,
                         PendingRecordRewardGrant& mutation) noexcept {
    mutation.beforeCharacter = before.characters[characterIndex];
    mutation.beforeProfileItems = before.profileItems;
    mutation.accountSoid = before.primarySoid;
    mutation.characterSoid = before.characters[characterIndex].soid;
    mutation.characterIndex = characterIndex;
    mutation.beforeProfileItemCount = before.profileItemCount;
}

/** Copies the batch's after-image and keeps the rows still present in it. */
void record_grant_after(const AccountState& after,
                        const PlacedRows& placed,
                        PendingRecordRewardGrant& mutation) noexcept {
    // A row a later change or eviction in the batch replaced is not announced.
    mutation.rowCount = 0;
    for (std::size_t index = 0; index < placed.count; ++index) {
        if (row_present(after, mutation.characterIndex, placed.values[index])) {
            mutation.rows[mutation.rowCount++] = placed.values[index];
        }
    }
    mutation.afterCharacter = after.characters[mutation.characterIndex];
    mutation.afterProfileItems = after.profileItems;
    mutation.afterProfileItemCount = after.profileItemCount;
    mutation.prepared = true;
}
} // namespace sunrise::state::runtime::detail
