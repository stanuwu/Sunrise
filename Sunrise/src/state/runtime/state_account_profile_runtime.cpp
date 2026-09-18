/** Profile inventory validation and material charges. */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "../../core/logging/log.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;

/** @return True when two profile stack rows carry identical authored values. */
[[nodiscard]] static bool same_profile_item(const authored_inventory::ProfileItem& left,
                                            const authored_inventory::ProfileItem& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.quantity == right.quantity && left.mutationSerial == right.mutationSerial
           && left.seen == right.seen;
}

/** @return True when a complete fixed profile inventory equals one captured view. */
[[nodiscard]] bool
same_profile_inventory(const AccountState& account,
                       const std::array<authored_inventory::ProfileItem,
                                        authored_inventory::kProfileItemCapacity>& expected,
                       std::size_t expectedCount) noexcept {
    if (account.profileItemCount != expectedCount) {
        return false;
    }
    for (std::size_t index = 0; index < account.profileItems.size(); ++index) {
        if (!same_profile_item(account.profileItems[index], expected[index])) {
            return false;
        }
    }
    return true;
}

/** @return True when two fixed profile views, including their empty tails, are identical. */
[[nodiscard]] bool same_profile_views(
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        left,
    std::size_t leftCount,
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        right,
    std::size_t rightCount) noexcept {
    if (leftCount != rightCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_profile_item(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

/** Validates dense profile stacks and their encoded bucket rows. */
[[nodiscard]] bool valid_profile_inventory(const AccountState& account) noexcept {
    if (account.profileItemCount > account.profileItems.size()) {
        return false;
    }
    // The bucket identity is one byte on the wire, so 256 covers every value one can carry.
    constexpr std::size_t kBucketIdentityCapacity = 256;
    std::array<std::uint16_t, kBucketIdentityCapacity> taken{};
    std::array<bool, inventory_buckets::kProfileSlotCapacity> occupied{};
    std::size_t actionSourceCount = 0;
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        const authored_inventory::ProfileItem& item = account.profileItems[index];
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (item.quantity <= 0 || item.mutationSerial < 0
            || !build_data::find_item_definition_hash(item.definitionHash, definition)
            || definition.definitionHash != item.definitionHash
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionIndex != definition.definitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile) {
            return false;
        }
        const bool actionSource =
            build_data::is_profile_action_source(definition.definitionIndex, definition.bucketId);
        if (actionSource != (item.instanceSoid != 0)
            || (actionSource
                && ++actionSourceCount > authored_inventory::kProfileActionSourceCapacity)) {
            return false;
        }
        const std::uint16_t used = taken[definition.bucketId];
        if (used >= bucket.slotCount) {
            return false;
        }
        const std::size_t row = static_cast<std::size_t>(bucket.firstSlot) + used;
        if (row >= occupied.size() || occupied[row]) {
            return false;
        }
        occupied[row] = true;
        taken[definition.bucketId] = static_cast<std::uint16_t>(used + 1U);
    }
    const auto tail =
        account.profileItems.cbegin() + static_cast<std::ptrdiff_t>(account.profileItemCount);
    return std::all_of(tail, account.profileItems.cend(), [](const auto& item) noexcept {
        return item.instanceSoid == 0 && item.definitionHash == 0 && item.quantity == 0
               && item.mutationSerial == 0;
    });
}

/** Validates a material set and applies deletions to a copied account. */
template <typename Requirement>
[[nodiscard]] static bool apply_material_requirements(const AccountState& before,
                                                      std::span<const Requirement> requirements,
                                                      AccountState& after,
                                                      bool& changed) noexcept {
    after = before;
    changed = false;
    if (requirements.size() > build_data::material_requirements::kRequirementCapacity) {
        return false;
    }

    struct MaterialCharge {
        std::uint32_t definitionHash{};
        std::uint64_t quantity{};
        bool deleteOnAction{};
    };
    std::array<MaterialCharge, build_data::material_requirements::kRequirementCapacity> charges{};
    std::size_t chargeCount = 0;
    for (const Requirement& requirement : requirements) {
        if (requirement.quantity == 0) {
            continue;
        }
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (requirement.itemDefinitionIndex
                == build_data::material_requirements::kUnavailableItemDefinitionIndex
            || !build_data::find_item_definition_index(requirement.itemDefinitionIndex, definition)
            || definition.definitionIndex != requirement.itemDefinitionIndex
            || !build_data::find_configured_item_detail(requirement.itemDefinitionIndex, detail)
            || detail.definitionIndex != requirement.itemDefinitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile
            || build_data::is_profile_action_source(definition.definitionIndex,
                                                    definition.bucketId)) {
            return false;
        }
        std::size_t chargeIndex = chargeCount;
        for (std::size_t existing = 0; existing < chargeCount; ++existing) {
            if (charges[existing].definitionHash == definition.definitionHash
                && charges[existing].deleteOnAction == requirement.deleteOnAction) {
                chargeIndex = existing;
                break;
            }
        }
        if (chargeIndex == chargeCount) {
            if (chargeCount >= charges.size()) {
                return false;
            }
            charges[chargeCount].definitionHash = definition.definitionHash;
            charges[chargeCount].deleteOnAction = requirement.deleteOnAction;
            ++chargeCount;
        }
        if (charges[chargeIndex].quantity
            > (std::numeric_limits<std::uint64_t>::max)() - requirement.quantity) {
            return false;
        }
        charges[chargeIndex].quantity += requirement.quantity;
    }

    for (std::size_t charge = 0; charge < chargeCount; ++charge) {
        std::uint64_t available = 0;
        for (std::size_t index = 0; index < before.profileItemCount; ++index) {
            const authored_inventory::ProfileItem& item = before.profileItems[index];
            if (item.definitionHash != charges[charge].definitionHash) {
                continue;
            }
            if (item.instanceSoid != 0 || item.quantity <= 0
                || available > (std::numeric_limits<std::uint64_t>::max)()
                                   - static_cast<std::uint64_t>(item.quantity)) {
                return false;
            }
            available += static_cast<std::uint64_t>(item.quantity);
        }
        if (available < charges[charge].quantity) {
            return false;
        }
    }

    std::array<std::uint64_t, build_data::material_requirements::kRequirementCapacity> remaining{};
    bool hasDeletion = false;
    for (std::size_t charge = 0; charge < chargeCount; ++charge) {
        if (charges[charge].deleteOnAction) {
            remaining[charge] = charges[charge].quantity;
            hasDeletion = true;
        }
    }
    if (!hasDeletion) {
        return true;
    }

    std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>
        compacted{};
    std::size_t compactedCount = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        authored_inventory::ProfileItem item = before.profileItems[index];
        for (std::size_t charge = 0; charge < chargeCount; ++charge) {
            if (remaining[charge] == 0 || item.definitionHash != charges[charge].definitionHash) {
                continue;
            }
            const auto available = static_cast<std::uint64_t>(item.quantity);
            const auto consumed = (std::min)(available, remaining[charge]);
            item.quantity -= static_cast<std::int32_t>(consumed);
            remaining[charge] -= consumed;
        }
        if (item.quantity != 0) {
            if (compactedCount >= compacted.size()) {
                return false;
            }
            compacted[compactedCount++] = item;
        }
    }
    if (std::any_of(remaining.cbegin(),
                    remaining.cbegin() + static_cast<std::ptrdiff_t>(chargeCount),
                    [](std::uint64_t value) noexcept { return value != 0; })) {
        return false;
    }

    std::int32_t greatestMutationSerial = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        greatestMutationSerial =
            (std::max)(greatestMutationSerial, before.profileItems[index].mutationSerial);
    }
    std::size_t changedRows = 0;
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index])) {
            ++changedRows;
        }
    }
    if (changedRows > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()
                                               - greatestMutationSerial)) {
        return false;
    }
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index])) {
            compacted[index].mutationSerial = ++greatestMutationSerial;
        }
    }
    after.profileItems = compacted;
    after.profileItemCount = compactedCount;
    changed = !same_profile_inventory(after, before.profileItems, before.profileItemCount);
    return changed && account::valid(after) && valid_profile_inventory(after);
}

/** Resolves one Collections row's installed cost without embedding any item or quantity policy. */
[[nodiscard]] bool
apply_collection_materials(const AccountState& before,
                           const build_data::collectibles::Definition& collectible,
                           AccountState& after,
                           bool& changed) noexcept {
    after = before;
    changed = false;
    if (collectible.materialRequirementCount == 0) {
        return collectible.materialRequirementSetIndex
                   == build_data::collectibles::kUnavailableMaterialRequirementSetIndex
               && collectible.materialRequirementSetHash == 0;
    }
    if (collectible.materialRequirementCount > collectible.materialRequirements.size()
        || collectible.materialRequirementSetIndex
               == build_data::collectibles::kUnavailableMaterialRequirementSetIndex
        || collectible.materialRequirementSetHash == 0) {
        return false;
    }
    return apply_material_requirements(
        before,
        std::span(collectible.materialRequirements)
            .first(static_cast<std::size_t>(collectible.materialRequirementCount)),
        after,
        changed);
}

/**
 * Writes what a refused price asked for, entry by entry, against what the profile holds.
 * A held count of -1 names an entry whose item is not installed, which the engine cannot debit.
 */
static void report_price_refusal(const AccountState& account,
                                 std::span<const build_data::vendors::SaleCost> price) noexcept {
    for (const build_data::vendors::SaleCost& entry : price) {
        build_data::items::Definition item{};
        std::int64_t held = -1;
        if (build_data::find_item_definition_index(entry.itemIndex, item)) {
            held = 0;
            for (std::size_t index = 0; index < account.profileItemCount; ++index) {
                if (account.profileItems[index].definitionHash == item.definitionHash) {
                    held += account.profileItems[index].quantity;
                }
            }
        }
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=vendor stage=price result=fail item=%u quantity=%u held=%lld",
                          static_cast<unsigned>(entry.itemIndex),
                          entry.quantity,
                          static_cast<long long>(held));
    }
}

/**
 * Spends one vendor sale row's price through the engine that spends Collections materials, so a
 * cost item is validated and debited by the same rules: a stackable profile item, every stack
 * summed, refused when short, then debited and compacted.
 * @param price The row's cost entries; empty charges nothing.
 * @param after Receives the charged account, or `before` again when refused.
 * @return True when every entry was paid, or the price was empty.
 */
[[nodiscard]] bool apply_sale_price(const AccountState& before,
                                    std::span<const build_data::vendors::SaleCost> price,
                                    AccountState& after,
                                    bool& changed) noexcept {
    namespace materials = build_data::material_requirements;
    after = before;
    changed = false;
    if (price.size() > build_data::vendors::kSaleCostCapacity) {
        return false;
    }
    // Every entry is consumed on purchase; nothing in a sale row is a held-only requirement.
    std::array<materials::Requirement, build_data::vendors::kSaleCostCapacity> requirements{};
    for (std::size_t index = 0; index < price.size(); ++index) {
        requirements[index] = {.quantity = price[index].quantity,
                               .itemDefinitionIndex = price[index].itemIndex,
                               .deleteOnAction = true};
    }
    if (apply_material_requirements(
            before,
            std::span<const materials::Requirement>{requirements.data(), price.size()},
            after,
            changed)) {
        return true;
    }
    after = before;
    changed = false;
    report_price_refusal(before, price);
    return false;
}

/** @return True when the account holds the requested socket-action source. */
[[nodiscard]] bool holds_plug_source(const AccountState& account,
                                     std::uint32_t definitionHash) noexcept {
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        const authored_inventory::ProfileItem& item = account.profileItems[index];
        if (item.definitionHash == definitionHash && item.quantity > 0) {
            return true;
        }
    }
    return false;
}

/** Spends one socket-action source unit and removes an emptied row. */
[[nodiscard]] bool spend_plug_source(AccountState& account, std::uint32_t definitionHash) noexcept {
    std::size_t row = account.profileItemCount;
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        if (account.profileItems[index].definitionHash == definitionHash
            && account.profileItems[index].quantity > 0) {
            row = index;
            break;
        }
    }
    if (row >= account.profileItemCount) {
        return false;
    }
    if (--account.profileItems[row].quantity > 0) {
        return true;
    }
    for (std::size_t index = row; index + 1U < account.profileItemCount; ++index) {
        account.profileItems[index] = account.profileItems[index + 1U];
    }
    account.profileItems[--account.profileItemCount] = {};
    return true;
}

/** Applies one dense installed action-cost set resolved from the selected plug or action row. */
[[nodiscard]] bool
apply_action_materials(const AccountState& before,
                       const build_data::material_requirements::Definition& definition,
                       AccountState& after,
                       bool& changed) noexcept {
    if (definition.requirementSetHash == 0
        || definition.requirementSetIndex == build_data::material_requirements::kUnavailableSetIndex
        || definition.requirementCount == 0
        || definition.requirementCount > definition.requirements.size()) {
        after = {};
        changed = false;
        return false;
    }
    for (std::size_t index = 0; index < definition.requirementCount; ++index) {
        if (definition.requirements[index].condition
            != build_data::material_requirements::kUnconditionalRequirement) {
            after = {};
            changed = false;
            return false;
        }
    }
    return apply_material_requirements(
        before,
        std::span(definition.requirements)
            .first(static_cast<std::size_t>(definition.requirementCount)),
        after,
        changed);
}

/** @return True when a pending profile acquisition carries canonical dense before/after images. */
[[nodiscard]] bool
valid_profile_mutation_shape(const PendingProfileItemAcquisition& mutation) noexcept {
    // The single-increment rules below pin a Collections pull. An exchange moves several rows by
    // more than one, so it has its own shape.
    if (mutation.changeCount != 0) {
        if (!mutation.prepared || mutation.accountSoid == 0 || mutation.actionSource
            || mutation.appended || mutation.acquiredInstanceSoid != 0
            || mutation.acquiredDefinitionHash == authored_inventory::kNoDefinitionHash
            || mutation.changeCount > mutation.changes.size()
            || mutation.expectedItemCount > authored_inventory::kProfileItemCapacity
            || mutation.afterItemCount > authored_inventory::kProfileItemCapacity
            || mutation.afterItemCount == 0) {
            return false;
        }
        for (std::size_t index = 0; index < mutation.beforeItems.size(); ++index) {
            const authored_inventory::ProfileItem& before = mutation.beforeItems[index];
            const authored_inventory::ProfileItem& after = mutation.afterItems[index];
            if (index >= mutation.expectedItemCount
                && (before.instanceSoid != 0 || before.definitionHash != 0 || before.quantity != 0
                    || before.mutationSerial != 0)) {
                return false;
            }
            if (index >= mutation.afterItemCount
                && (after.instanceSoid != 0 || after.definitionHash != 0 || after.quantity != 0
                    || after.mutationSerial != 0)) {
                return false;
            }
        }
        // The change ring points at rows by serial, so each announced serial must name exactly one
        // after-image row carrying the quantity the change names.
        for (std::size_t change = 0; change < mutation.changeCount; ++change) {
            const ProfileStackChange& announced = mutation.changes[change];
            if (announced.mutationSerial <= 0 || announced.afterQuantity <= 0) {
                return false;
            }
            // Two changes naming one row would announce the same gain twice, and the ring has no
            // way to say they meant different things.
            for (std::size_t earlier = 0; earlier < change; ++earlier) {
                if (mutation.changes[earlier].mutationSerial == announced.mutationSerial) {
                    return false;
                }
            }
            std::size_t matches = 0;
            for (std::size_t index = 0; index < mutation.afterItemCount; ++index) {
                if (mutation.afterItems[index].mutationSerial != announced.mutationSerial) {
                    continue;
                }
                if (mutation.afterItems[index].quantity != announced.afterQuantity) {
                    return false;
                }
                ++matches;
            }
            if (matches != 1) {
                return false;
            }
        }
        return true;
    }
    if (!mutation.prepared || mutation.accountSoid == 0
        || mutation.actionSource != (mutation.acquiredInstanceSoid != 0)
        || mutation.acquiredDefinitionHash == authored_inventory::kNoDefinitionHash
        || mutation.expectedItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterItemCount > authored_inventory::kProfileItemCapacity
        || mutation.profileIndex >= mutation.afterItemCount || mutation.previousQuantity < 0
        || mutation.acquiredQuantity <= mutation.previousQuantity
        || (!mutation.directGrant && mutation.acquiredQuantity - mutation.previousQuantity != 1)
        || mutation.previousMutationSerial < 0
        || mutation.acquiredMutationSerial <= mutation.previousMutationSerial) {
        return false;
    }
    if (mutation.appended) {
        if (mutation.afterItemCount == 0 || mutation.previousQuantity != 0
            || (mutation.directGrant
                && (mutation.afterItemCount != mutation.expectedItemCount + 1U
                    || mutation.profileIndex != mutation.expectedItemCount))) {
            return false;
        }
    } else if (mutation.previousQuantity == 0
               || (mutation.directGrant && mutation.afterItemCount != mutation.expectedItemCount)) {
        return false;
    }

    bool foundBeforeTarget = mutation.appended;
    for (std::size_t index = 0; index < mutation.beforeItems.size(); ++index) {
        const authored_inventory::ProfileItem& before = mutation.beforeItems[index];
        const authored_inventory::ProfileItem& after = mutation.afterItems[index];
        if (mutation.directGrant && index != mutation.profileIndex
            && !same_profile_item(before, after)) {
            return false;
        }
        if (index < mutation.expectedItemCount
            && before.mutationSerial >= mutation.acquiredMutationSerial) {
            return false;
        }
        if (index >= mutation.expectedItemCount
            && (before.instanceSoid != 0 || before.definitionHash != 0 || before.quantity != 0
                || before.mutationSerial != 0)) {
            return false;
        }
        if (index >= mutation.afterItemCount
            && (after.instanceSoid != 0 || after.definitionHash != 0 || after.quantity != 0
                || after.mutationSerial != 0)) {
            return false;
        }
        if (!mutation.appended && index < mutation.expectedItemCount
            && before.instanceSoid == mutation.acquiredInstanceSoid
            && before.definitionHash == mutation.acquiredDefinitionHash
            && before.quantity == mutation.previousQuantity
            && before.mutationSerial == mutation.previousMutationSerial) {
            if (foundBeforeTarget) {
                return false;
            }
            foundBeforeTarget = true;
        }
    }
    const authored_inventory::ProfileItem& acquired = mutation.afterItems[mutation.profileIndex];
    return foundBeforeTarget && acquired.instanceSoid == mutation.acquiredInstanceSoid
           && acquired.definitionHash == mutation.acquiredDefinitionHash
           && acquired.quantity == mutation.acquiredQuantity
           && acquired.mutationSerial == mutation.acquiredMutationSerial;
}

/** Applies one validated pending profile after-image over a current, matching account. */
[[nodiscard]] bool materialize_profile_acquisition(const AccountState& current,
                                                   const PendingProfileItemAcquisition& mutation,
                                                   AccountState& after) noexcept {
    if (!valid_profile_mutation_shape(mutation) || current.primarySoid != mutation.accountSoid
        || !same_profile_inventory(current, mutation.beforeItems, mutation.expectedItemCount)) {
        return false;
    }
    // An exchange names no collectible, bucket or single acquired row, so the acquisition's
    // definition checks do not apply. The shape check above already covered its after-image.
    if (mutation.changeCount != 0) {
        after = current;
        after.profileItems = mutation.afterItems;
        after.profileItemCount = mutation.afterItemCount;
        return account::valid(after) && valid_profile_inventory(after);
    }
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_hash(mutation.acquiredDefinitionHash, item)) {
        return false;
    }
    if (mutation.directGrant) {
        // Direct rewards have no collectible or material source.
        if (mutation.collectibleIndex != 0 || mutation.materialRequirementSetHash != 0
            || mutation.materialRequirementCount != 0) {
            return false;
        }
    } else if (mutation.collectibleIndex == build_data::collectibles::kNoCollectibleIndex) {
        // A vendor purchase names an item, never a collectible, and arrives with the sentinel.
        // With no collectible to hold them, both cost fields must still be clear.
        if (mutation.materialRequirementSetHash != 0 || mutation.materialRequirementCount != 0) {
            return false;
        }
    } else {
        build_data::collectibles::Definition collectible{};
        if (!build_data::find_collectible_definition(mutation.collectibleIndex, collectible)
            || collectible.itemDefinitionIndex
                   == build_data::collectibles::kUnavailableItemDefinitionIndex
            || collectible.materialRequirementSetHash != mutation.materialRequirementSetHash
            || collectible.materialRequirementCount != mutation.materialRequirementCount
            || collectible.itemDefinitionIndex != item.definitionIndex) {
            return false;
        }
    }
    if (!build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionHash != mutation.acquiredDefinitionHash
        || detail.definitionIndex != item.definitionIndex || detail.bucketId != item.bucketId
        || detail.bucketId != mutation.bucketId
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || detail.maxStackSize <= 0 || mutation.acquiredQuantity > detail.maxStackSize
        || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
        || bucket.arraySelector != inventory_buckets::ArraySelector::profile
        || build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
               != mutation.actionSource) {
        return false;
    }
    after = current;
    after.profileItems = mutation.afterItems;
    after.profileItemCount = mutation.afterItemCount;
    return account::valid(after) && valid_profile_inventory(after);
}

} // namespace runtime::detail

/** Prepares one vendor recycle row: charges the stack it names and credits what it pays out. */
bool prepare_vendor_exchange(std::uint32_t costDefinitionHash,
                             std::int32_t costQuantity,
                             std::span<const ProfileExchangePayout> payouts,
                             PendingProfileItemAcquisition& mutation) noexcept {
    namespace authored_inventory = account::inventory;
    namespace item_details = build_data::items::details;
    mutation = {};
    if (costDefinitionHash == authored_inventory::kNoDefinitionHash || costDefinitionHash == 0
        || costQuantity <= 0 || payouts.empty() || payouts.size() > kProfileStackChangeCapacity) {
        return false;
    }
    const AccountState account = account_snapshot();
    if (!account::valid(account) || account.primarySoid == 0) {
        return false;
    }
    const auto stack_limit = [](std::uint32_t definitionHash, std::int32_t& limit) noexcept {
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        if (!build_data::find_item_definition_hash(definitionHash, definition)
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.maxStackSize <= 0) {
            return false;
        }
        limit = detail.maxStackSize;
        return true;
    };
    const auto find_stack = [](const AccountState& state, std::uint32_t definitionHash) noexcept {
        std::size_t at = state.profileItemCount;
        for (std::size_t index = 0; index < state.profileItemCount; ++index) {
            if (state.profileItems[index].definitionHash == definitionHash) {
                at = index;
                break;
            }
        }
        return at;
    };

    AccountState after = account;
    const std::size_t costIndex = find_stack(after, costDefinitionHash);
    if (costIndex >= after.profileItemCount
        || after.profileItems[costIndex].quantity < costQuantity) {
        return false;
    }
    // The charged row keeps its ordering token: only a gain is announced, and a fresh serial would
    // move the charged stack to the front of its bucket.
    after.profileItems[costIndex].quantity -= costQuantity;

    // Serials rise from the greatest already in the profile, so every announced row is unique and
    // no existing row is displaced in the Client's ordering.
    std::int32_t serial = 0;
    for (std::size_t index = 0; index < after.profileItemCount; ++index) {
        serial = (std::max)(serial, after.profileItems[index].mutationSerial);
    }
    if (serial
        > (std::numeric_limits<std::int32_t>::max)() - static_cast<std::int32_t>(payouts.size())) {
        return false;
    }
    std::size_t changeCount = 0;
    for (const ProfileExchangePayout& payout : payouts) {
        std::int32_t limit = 0;
        const std::size_t at = find_stack(after, payout.definitionHash);
        // Paying back into the charged stack is refused, not netted out: the charge removes an
        // emptied row, so a later credit would land on a row about to leave the array.
        if (payout.quantity <= 0 || payout.definitionHash == costDefinitionHash
            || !stack_limit(payout.definitionHash, limit) || at >= after.profileItemCount) {
            return false;
        }
        // A currency already at its native cap takes nothing, which is the same outcome the Client
        // reports as "your Glimmer is full" rather than a failed exchange.
        const std::int32_t room = (std::max)(limit - after.profileItems[at].quantity, 0);
        const std::int32_t credited = (std::min)(payout.quantity, room);
        if (credited == 0) {
            continue;
        }
        after.profileItems[at].quantity += credited;
        after.profileItems[at].mutationSerial = ++serial;
        mutation.changes[changeCount++] = {after.profileItems[at].mutationSerial,
                                           after.profileItems[at].quantity};
    }
    // Nothing to announce means nothing was credited, and charging for that would be theft.
    if (changeCount == 0) {
        return false;
    }
    // A stack the charge emptied has to leave the array, because a zero-quantity row is not a valid
    // profile row. It is removed last so the credited rows above were found at their real indices.
    if (after.profileItems[costIndex].quantity == 0) {
        for (std::size_t index = costIndex; index + 1U < after.profileItemCount; ++index) {
            after.profileItems[index] = after.profileItems[index + 1U];
        }
        --after.profileItemCount;
        after.profileItems[after.profileItemCount] = {};
    }
    if (!account::valid(after) || !runtime::detail::valid_profile_inventory(after)) {
        return false;
    }

    mutation.beforeItems = account.profileItems;
    mutation.afterItems = after.profileItems;
    mutation.accountSoid = account.primarySoid;
    mutation.acquiredDefinitionHash = costDefinitionHash;
    mutation.expectedItemCount = account.profileItemCount;
    mutation.afterItemCount = after.profileItemCount;
    mutation.changeCount = changeCount;
    mutation.prepared = true;
    return true;
}

} // namespace sunrise::state
