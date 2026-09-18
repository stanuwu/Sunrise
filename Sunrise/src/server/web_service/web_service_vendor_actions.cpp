/** Vendor, bounty, exchange and quest actions the web service prepares from one request. */

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode1820.h"
#include "../../middleware/web_service/messages/opcode901/opcode901_codec.h"
#include "../../middleware/web_service/messages/opcode904/opcode904_codec.h"
#include "../../state/account/account_state.h"
#include "../../state/account/pursuit_hold.h"
#include "../../state/build_data/bounties/definition.h"
#include "../../state/build_data/items/item_catalog.h"
#include "../../state/build_data/runtime.h"
#include "../../state/build_data/vendors/repeatable_triggers.h"
#include "../../state/build_data/vendors/vendor_catalog.h"
#include "../../state/runtime/runtime.h"
#include "internal_actions.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {

namespace {

/** Index stored when no definition resolves. The catalog is u16-indexed, so this cannot be one. */
constexpr std::uint32_t kUnavailableDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
/** Repeatable bounties a character may hold from one vendor at once, as retail allows. */
constexpr std::uint32_t kRepeatableHoldLimit = 5;

/** Item index every synth recycle row sells. The row recycles; it never hands this over. */
constexpr std::uint16_t kRecycleSynthsItemIndex = 7152;
/** Item index every shader recycle row sells. The row recycles; it never hands this over. */
constexpr std::uint16_t kRecycleShadersItemIndex = 5311;
/** Glimmer, the currency both recycle kinds pay in. */
constexpr std::uint32_t kGlimmerHash = 0xBC53E66EU;
/** Legendary Shards, which only shader recycling pays. */
constexpr std::uint32_t kLegendaryShardHash = 0x3CF2E8E2U;
/** Glimmer one synth recycle row pays. Server policy: no package field carries a payout. */
constexpr std::int32_t kSynthRecycleGlimmer = 100;
/** Glimmer one shader recycle row pays. Server policy, as above. */
constexpr std::int32_t kShaderRecycleGlimmer = 250;
/** Legendary Shards one shader recycle row pays. Server policy, as above. */
constexpr std::int32_t kShaderRecycleShards = 5;
/** Stacks one exchange row credits. Shader recycling pays two: Glimmer and Legendary Shards. */
constexpr std::size_t kExchangePayoutCapacity = 2;
// Every credited stack is announced to the account's change ring, so a kind that paid more than
// the mutation can announce would pay out silently. Raising one raises the other.
static_assert(kExchangePayoutCapacity <= state::kProfileStackChangeCapacity);

} // namespace

/** Reports an opcode-1820 validation failure. */
void report_item_acquisition(const middleware::web_service::Message& message,
                             std::string_view reason,
                             std::uint32_t collectibleIndex,
                             std::uint32_t itemDefinitionIndex,
                             std::uint32_t definitionHash,
                             std::uint64_t instanceSoid) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1820 stage=prepare result=fail reason=%.*s transaction=%u payload_bytes=%zu "
        "collectible_index=%u item_definition_index=%u definition_hash=0x%08X instance=0x%llX",
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        collectibleIndex,
        itemDefinitionIndex,
        definitionHash,
        static_cast<unsigned long long>(instanceSoid));
    write_warning(line, count);
}

/**
 * Writes one purchase line. 901 and 904 share it, so the opcode is carried, not hard-coded.
 * @param opcode Request opcode the line belongs to, 901 or 904.
 * @param result `ok` or `fail`.
 * @param reason Step that decided it.
 * @param vendorIndex Vendor row the request named.
 * @param saleIndex Sale row the request named.
 * @param itemDefinitionIndex Item resolved, when the row resolved.
 */
void report_purchase(std::uint16_t opcode,
                     const char* result,
                     const char* reason,
                     std::int32_t vendorIndex,
                     std::int32_t saleIndex,
                     std::uint16_t itemDefinitionIndex) noexcept {
    core::log::writef(core::log::Channel::server,
                      std::strcmp(result, "ok") == 0 ? core::log::Level::info
                                                     : core::log::Level::warn,
                      "ev=ws%u stage=purchase result=%s reason=%s vendor=%d sale=%d item=%u",
                      static_cast<unsigned>(opcode),
                      result,
                      reason,
                      static_cast<int>(vendorIndex),
                      static_cast<int>(saleIndex),
                      static_cast<unsigned>(itemDefinitionIndex));
}

/**
 * Resolves the vendor a request names to its index row and held definition.
 * A negative index is the client's absent marker and never a row.
 * @param vendorIndex Vendor row the request named.
 * @param entry Receives the index row.
 * @param definition Receives the held definition.
 * @return True when the row exists and its definition is published.
 */
[[nodiscard]] bool find_vendor(std::int32_t vendorIndex,
                               state::build_data::vendors::IndexEntry& entry,
                               state::build_data::vendors::Definition& definition) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    entry = {};
    definition = {};
    return vendorIndex >= 0 && vendorIndex <= (std::numeric_limits<std::uint16_t>::max)()
           && vendor_domain::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
           && vendor_domain::find(entry.definitionHash, definition);
}

/** What the substitution table said about one sale row's item. */
enum class Substitution : std::uint8_t {
    /** The table does not name this item; the row grants what it names. */
    none,
    /** The table names it and its replacement resolved; the row grants the replacement. */
    replaced,
    /** The table names it but its replacement is not in this build; the row grants nothing. */
    broken,
};

/** One placeholder sale item and the item buying it really hands over. */
struct LegacyQuestStep {
    std::uint32_t soldHash{};
    std::uint32_t grantHash{};
};

/**
 * Amanda Holliday's three Legacy rows. Each sells a placeholder standing for a campaign's first
 * quest step, and no package field links the two, so each pair is named here.
 */
constexpr std::array<LegacyQuestStep, 3> kLegacyQuestSteps{{
    // Legacy: The Red War -> Homecoming
    {0xBEB63647U, 0x37DD26F0U},
    // Legacy: Curse of Osiris -> The Gateway
    {0x6CBEA754U, 0x6706D3ECU},
    // Legacy: Warmind -> Ice and Shadow
    {0x65683247U, 0xF5B78E7FU},
}};

/**
 * Answers what a placeholder sale row is really selling.
 * A named row whose replacement is missing answers `broken`: being named proves the row's item is
 * a placeholder, so granting it would put an undrawable item in the inventory.
 * @param itemDefinitionIndex Item the row resolved to.
 * @param substituteIndex Receives what should be granted in its place.
 * @return What the substitution table said about this item.
 */
[[nodiscard]] Substitution substitute_for_item(std::uint16_t itemDefinitionIndex,
                                               std::uint16_t& substituteIndex) noexcept {
    substituteIndex = kUnavailableDefinitionIndex;
    state::build_data::items::Definition sold{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, sold)) {
        return Substitution::none;
    }
    for (const LegacyQuestStep& step : kLegacyQuestSteps) {
        if (step.soldHash != sold.definitionHash) {
            continue;
        }
        state::build_data::items::Definition replacement{};
        if (state::build_data::find_item_definition_hash(step.grantHash, replacement)) {
            substituteIndex = replacement.definitionIndex;
            core::log::writef(core::log::Channel::server,
                              core::log::Level::info,
                              "ev=vendor stage=substitute sold=0x%08X granted=0x%08X item=%u",
                              sold.definitionHash,
                              replacement.definitionHash,
                              static_cast<unsigned>(replacement.definitionIndex));
            return Substitution::replaced;
        }
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=vendor stage=substitute result=fail reason=missing sold=0x%08X "
                          "named=0x%08X",
                          sold.definitionHash,
                          step.grantHash);
        return Substitution::broken;
    }
    return Substitution::none;
}

/**
 * Rolls one random unheld repeatable bounty, for a row that offers "Additional Bounties".
 * A repeatable appears in no vendor's sale list, so the trigger names the item-type its pool
 * shares and the bounty catalog lists every item carrying that pair.
 * @param vendorIndex Vendor the purchase names.
 * @param categoryIndex Category of the purchased row, from sale row +100.
 * @param rolledItemIndex Receives the bounty to grant.
 * @return True when this row is a bounty roll and its own item must NOT be granted.
 */
[[nodiscard]] bool roll_vendor_bounty(std::int32_t vendorIndex,
                                      std::int32_t categoryIndex,
                                      std::uint16_t& rolledItemIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    rolledItemIndex = kUnavailableDefinitionIndex;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (categoryIndex < 0 || !find_vendor(vendorIndex, entry, definition)) {
        return false;
    }
    const vendor_domain::RepeatableTrigger* const trigger =
        vendor_domain::find_repeatable_trigger(entry.definitionHash, categoryIndex);
    if (trigger == nullptr) {
        return false;
    }
    std::array<std::uint16_t, state::build_data::bounties::kPoolCapacity> pool{};
    std::size_t poolCount = 0;
    if (!state::build_data::repeatable_bounty_pool(trigger->itemType, pool, poolCount)) {
        return false;
    }
    // Reservoir pick over what the character does not already hold, so the pool is walked once and
    // no candidate count is needed up front.
    std::uint32_t held = 0;
    std::uint32_t candidates = 0;
    std::uint64_t seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    // One account view for the whole pool. Reading it copies the whole account, and the pool is
    // walked candidate by candidate, so taking it per candidate would copy it dozens of times to
    // answer dozens of questions about the same unchanging view.
    const state::AccountState account = state::account_snapshot();
    for (std::size_t at = 0; at < poolCount; ++at) {
        if (state::account::holds_pursuit(account, pool[at])) {
            ++held;
            continue;
        }
        ++candidates;
        seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
        if ((seed >> 33) % candidates == 0) {
            rolledItemIndex = pool[at];
        }
    }
    // Retail lets a character keep five of a vendor's repeatables at once. Refusing here rather
    // than at the grant keeps the roll from consuming a pick it would only have to throw away.
    if (held >= kRepeatableHoldLimit) {
        rolledItemIndex = kUnavailableDefinitionIndex;
    }
    core::log::writef(
        core::log::Channel::server,
        core::log::Level::info,
        "ev=bounty_roll stage=pick vendor=%d hash=0x%08X category=%d pool=%u "
        "held=%u candidates=%u item=%d",
        vendorIndex,
        entry.definitionHash,
        categoryIndex,
        static_cast<unsigned>(poolCount),
        held,
        candidates,
        rolledItemIndex == kUnavailableDefinitionIndex ? -1 : static_cast<int>(rolledItemIndex));
    return true;
}

/**
 * Runs a vendor's recycle row: charges the stack the row names and credits what it pays out.
 * The row's own item is the placeholder that names the recycle kind, and the kind sets the payout.
 * @param vendorIndex Vendor the purchase names.
 * @param rowIndex Sale row the purchase names.
 * @param mutation Receives the prepared profile-stack change.
 * @return True when this row was an exchange and its own item must NOT be granted.
 */
[[nodiscard]] bool exchange_vendor_row(std::int32_t vendorIndex,
                                       std::int32_t rowIndex,
                                       state::PendingProfileItemAcquisition& mutation) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (rowIndex < 0 || !find_vendor(vendorIndex, entry, definition)) {
        return false;
    }
    vendor_domain::SaleRow row{};
    if (!vendor_domain::sale_row(definition, static_cast<std::size_t>(rowIndex), row)) {
        return false;
    }
    std::array<state::ProfileExchangePayout, kExchangePayoutCapacity> payouts{};
    std::size_t payoutCount = 0;
    if (row.itemIndex == kRecycleSynthsItemIndex) {
        payouts[payoutCount++] = {kGlimmerHash, kSynthRecycleGlimmer};
    } else if (row.itemIndex == kRecycleShadersItemIndex) {
        payouts[payoutCount++] = {kGlimmerHash, kShaderRecycleGlimmer};
        payouts[payoutCount++] = {kLegendaryShardHash, kShaderRecycleShards};
    } else {
        return false;
    }
    // A sale row holds its cost as u32; the mutation charges an i32, so a wider row is refused.
    constexpr auto kQuantityLimit =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    // An exchange charges exactly one static stack, so a row that is not plainly priced, or
    // declares any number of entries but one, is not one.
    const std::span<const vendor_domain::SaleCost> price = vendor_domain::cost_entries(row);
    const vendor_domain::SaleCost single = price.size() == 1 ? price[0] : vendor_domain::SaleCost{};
    state::build_data::items::Definition cost{};
    // A recycle row owns its purchase from here, refused or not: falling through would grant the
    // placeholder, which is the failure this path exists to avoid.
    if (row.priceState != vendor_domain::PriceState::plain || price.size() != 1
        || single.quantity == 0 || single.quantity > kQuantityLimit
        || !state::build_data::find_item_definition_index(single.itemIndex, cost)) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=vendor_exchange stage=apply result=fail reason=cost vendor=%d "
                          "hash=0x%08X row=%d price_state=%u entries=%zu cost_item=%u "
                          "quantity=%u",
                          vendorIndex,
                          entry.definitionHash,
                          rowIndex,
                          static_cast<unsigned>(row.priceState),
                          price.size(),
                          static_cast<unsigned>(single.itemIndex),
                          static_cast<unsigned>(single.quantity));
        return true;
    }
    const bool applied = state::prepare_vendor_exchange(
        cost.definitionHash,
        static_cast<std::int32_t>(single.quantity),
        std::span<const state::ProfileExchangePayout>{payouts.data(), payoutCount},
        mutation);
    core::log::writef(core::log::Channel::server,
                      applied ? core::log::Level::info : core::log::Level::warn,
                      "ev=vendor_exchange stage=apply result=%s vendor=%d hash=0x%08X row=%d "
                      "cost=0x%08X quantity=%u payouts=%zu",
                      applied ? "ok" : "fail",
                      vendorIndex,
                      entry.definitionHash,
                      rowIndex,
                      cost.definitionHash,
                      static_cast<unsigned>(single.quantity),
                      payoutCount);
    return true;
}

/**
 * Grants one item, given the collectible that owns it and its definition index.
 * Acquisition state is keyed by collectible, so the caller resolves one first.
 * @param message Request being answered, for the log line.
 * @param collectibleIndex Collectible that owns the item.
 * @param itemDefinitionIndex Item to grant.
 * @param price A vendor row's cost entries, spent in place of the collectible's material set;
 *        absent for a Collections pull, which pays with the collectible's materials.
 * @param outcome Receives the prepared mutation on success.
 * @return True when a mutation is prepared. A pursuit already held prepares none.
 */
bool grant_item_definition(
    const middleware::web_service::Message& message,
    std::uint16_t collectibleIndex,
    std::uint16_t itemDefinitionIndex,
    std::optional<std::span<const state::build_data::vendors::SaleCost>> price,
    Outcome& outcome) noexcept {
    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, definition)) {
        report_item_acquisition(
            message, "item_definition", collectibleIndex, itemDefinitionIndex, 0, 0);
        return false;
    }
    // The same rule the client's native vendor-row gate applies locally, so a row that is still
    // offered can never be one this grant would refuse.
    if (state::account::holds_pursuit(itemDefinitionIndex)) {
        report_item_acquisition(message,
                                "already_held",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return false;
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        report_item_acquisition(message,
                                "item_detail_or_bucket",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return false;
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            report_item_acquisition(message,
                                    "profile_item_instanced",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return false;
        }
        auto* mutation = emplace_mutation<state::PendingProfileItemAcquisition>(outcome);
        if (mutation == nullptr) {
            report_item_acquisition(message,
                                    "storage",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return false;
        }
        if (!state::prepare_profile_item_acquisition(
                collectibleIndex, definition.definitionHash, price, *mutation)) {
            clear_mutation(outcome);
            report_item_acquisition(message,
                                    "profile_state",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return false;
        }
        return true;
    }
    if (bucket.arraySelector != bucket_domain::ArraySelector::character) {
        report_item_acquisition(message,
                                "unsupported_inventory_array",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return false;
    }

    auto* mutation = emplace_mutation<state::PendingItemAcquisition>(outcome);
    if (mutation == nullptr) {
        report_item_acquisition(message,
                                "storage",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return false;
    }
    if (!state::prepare_item_acquisition(
            collectibleIndex, definition.definitionHash, price, *mutation)) {
        clear_mutation(outcome);
        report_item_acquisition(
            message, "state", collectibleIndex, itemDefinitionIndex, definition.definitionHash, 0);
        return false;
    }
    return true;
}

/**
 * Finds the collectible that owns one item definition.
 * Bounties, tokens and quest steps have none, so the caller's sentinel is left in place and the
 * grant runs by hash under `kNoCollectibleIndex`.
 * @param itemDefinitionIndex Item to look up.
 * @param collectibleIndex Receives the owning collectible row; untouched when none does.
 * @return True when a collectible names this item.
 */
[[nodiscard]] bool find_collectible_for_item(std::uint16_t itemDefinitionIndex,
                                             std::uint16_t& collectibleIndex) noexcept {
    return state::build_data::collectibles::find_granting(itemDefinitionIndex, collectibleIndex);
}

/** Prepares the exact three-byte opcode-1820 Collections item request. */
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode1820::Request request{};
    if (!middleware::web_service::messages::opcode1820::parse_request(message, request)) {
        report_item_acquisition(message,
                                "payload_bits",
                                kUnavailableDefinitionIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }
    const std::uint16_t collectibleIndex = request.collectibleIndex;
    std::uint16_t itemDefinitionIndex = 0;
    if (!state::build_data::find_collectible_item_definition_index(collectibleIndex,
                                                                   itemDefinitionIndex)) {
        report_item_acquisition(
            message, "collectible_definition", collectibleIndex, kUnavailableDefinitionIndex, 0, 0);
        return;
    }
    (void)grant_item_definition(
        message, collectibleIndex, itemDefinitionIndex, std::nullopt, outcome);
}

/**
 * Resolves one vendor row to the item it sells. Shared by 901 and 904, which name a row alike.
 * @param vendorIndex Vendor table row.
 * @param rowIndex Sale row within that vendor.
 * @param row Receives the sale row: its item, category and price.
 * @param reason Receives the step that failed, when one does.
 * @return True when the row resolved.
 */
[[nodiscard]] bool resolve_vendor_row(std::int32_t vendorIndex,
                                      std::int32_t rowIndex,
                                      state::build_data::vendors::SaleRow& row,
                                      const char*& reason) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    row = {};
    if (vendorIndex < 0 || rowIndex < 0) {
        reason = "negative_index";
        return false;
    }
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (!find_vendor(vendorIndex, entry, definition)) {
        reason = "vendor";
        return false;
    }
    if (!vendor_domain::sale_row(definition, static_cast<std::size_t>(rowIndex), row)) {
        reason = "sale_row";
        return false;
    }
    return true;
}

/** Pursuit rows written out when a vendor is asked what it actually sells. */
constexpr std::size_t kPursuitListCap = 64;

/**
 * Lists the sale rows of one vendor whose item is a pursuit, when a rowless tile fails to resolve.
 * Reported by item, not by row, because one placeholder repeats across dozens of rows.
 * @param vendorIndex Vendor to list.
 */
void report_pursuit_rows(std::int32_t vendorIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    namespace detail_domain = state::build_data::items::details;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (!find_vendor(vendorIndex, entry, definition)) {
        return;
    }
    const std::size_t count = definition.saleCount;
    // One placeholder item repeats across dozens of rows, so only distinct items are listed.
    std::array<std::uint16_t, kPursuitListCap> seen{};
    std::size_t listed = 0;
    std::size_t pursuits = 0;
    for (std::size_t row = 0; row < count; ++row) {
        vendor_domain::SaleRow sale{};
        if (!vendor_domain::sale_row(definition, row, sale)) {
            break;
        }
        const std::uint16_t itemIndex = sale.itemIndex;
        detail_domain::Definition detail{};
        if (!state::build_data::find_configured_item_detail(itemIndex, detail)
            || detail.equipmentSlot.has_value() || detail.maxStackSize > 1) {
            continue;
        }
        ++pursuits;
        bool duplicate = false;
        for (std::size_t index = 0; index < listed; ++index) {
            duplicate = duplicate || seen[index] == itemIndex;
        }
        if (duplicate || listed >= kPursuitListCap) {
            continue;
        }
        seen[listed] = itemIndex;
        ++listed;
        core::log::writef(core::log::Channel::server,
                          core::log::Level::debug,
                          "ev=vendor stage=pursuit vendor=%d sale=%zu item=%u hash=0x%08X "
                          "bucket=%u",
                          static_cast<int>(vendorIndex),
                          row,
                          static_cast<unsigned>(itemIndex),
                          detail.definitionHash,
                          static_cast<unsigned>(detail.bucketId));
    }
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=vendor stage=pursuits vendor=%d sale_rows=%zu pursuits=%zu "
                      "distinct_listed=%zu",
                      static_cast<int>(vendorIndex),
                      count,
                      pursuits,
                      listed);
}

/** FNV-1's basis, which this engine also uses as its absent-hash sentinel. */
constexpr std::uint32_t kAbsentNameHash = 0x811C9DC5U;

/**
 * Resolves the item behind a 904 that names no sale row.
 * The slot indexes the vendor's category array, not its sale rows, and a category row carries the
 * item's definition hash where a sale row names an index.
 * @param vendorIndex Vendor the request named.
 * @param slotIndex The 16-bit slot field, which is all the request carries.
 * @param itemDefinitionIndex Receives the item, or the unavailable sentinel.
 * @return True when the row's hash resolved to an installed item definition.
 */
[[nodiscard]] bool resolve_rowless_quest(std::int32_t vendorIndex,
                                         std::int32_t slotIndex,
                                         std::uint16_t& itemDefinitionIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    itemDefinitionIndex = kUnavailableDefinitionIndex;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (slotIndex < 0 || !find_vendor(vendorIndex, entry, definition)) {
        return false;
    }
    vendor_domain::InstalledRow installed{};
    if (!vendor_domain::installed_row(definition, static_cast<std::size_t>(slotIndex), installed)) {
        return false;
    }
    const std::uint32_t definitionHash = installed.definitionHash;
    state::build_data::items::Definition item{};
    const bool resolved = definitionHash != kAbsentNameHash
                          && state::build_data::find_item_definition_hash(definitionHash, item);
    if (resolved) {
        itemDefinitionIndex = item.definitionIndex;
    }
    core::log::writef(core::log::Channel::server,
                      resolved ? core::log::Level::info : core::log::Level::warn,
                      "ev=ws904 stage=rowless vendor=%d slot=%d installed=%u sale=%u third=%u "
                      "hash=0x%08X item=%u resolved=%u",
                      static_cast<int>(vendorIndex),
                      static_cast<int>(slotIndex),
                      static_cast<unsigned>(definition.installedCount),
                      static_cast<unsigned>(definition.saleCount),
                      static_cast<unsigned>(definition.thirdCount),
                      definitionHash,
                      static_cast<unsigned>(itemDefinitionIndex),
                      resolved ? 1U : 0U);
    return resolved;
}

/**
 * Settles one resolved vendor row, in the order a row's behaviours are tried.
 * A row is a bounty roll, an exchange or a grant. The row does not say which, so each is tried in
 * turn and the first that claims the row owns it. Both vendor opcodes end here.
 * @param message Request being answered.
 * @param opcode Opcode to report under.
 * @param vendorIndex Vendor the request names.
 * @param rowIndex Sale row the request names.
 * @param row The resolved sale row. Its price is spent by a grant and by a bounty roll, and an
 *        exchange spends it as the stack it recycles.
 * @param outcome Receives whatever mutation the row prepared.
 */
void settle_vendor_row(const middleware::web_service::Message& message,
                       std::uint16_t opcode,
                       std::int32_t vendorIndex,
                       std::int32_t rowIndex,
                       const state::build_data::vendors::SaleRow& row,
                       Outcome& outcome) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    const std::int32_t categoryIndex = row.categoryIndex;
    const std::uint16_t itemDefinitionIndex = row.itemIndex;
    // A row is sold only while its static entries are its price. A conditional row's price
    // depends on state this build does not evaluate, and an unreadable row's is unknown.
    if (row.priceState != vendor_domain::PriceState::plain) {
        report_purchase(opcode,
                        "fail",
                        row.priceState == vendor_domain::PriceState::conditional
                            ? "conditional_price"
                            : "unreadable_price",
                        vendorIndex,
                        rowIndex,
                        itemDefinitionIndex);
        return;
    }
    const std::span<const vendor_domain::SaleCost> price = vendor_domain::cost_entries(row);
    std::uint16_t rolledBounty = kUnavailableDefinitionIndex;
    if (roll_vendor_bounty(vendorIndex, categoryIndex, rolledBounty)) {
        report_purchase(opcode,
                        "ok",
                        rolledBounty == kUnavailableDefinitionIndex ? "bounty_pool_empty"
                                                                    : "bounty_roll",
                        vendorIndex,
                        rowIndex,
                        itemDefinitionIndex);
        if (rolledBounty != kUnavailableDefinitionIndex) {
            std::uint16_t rolledCollectible = state::build_data::collectibles::kNoCollectibleIndex;
            (void)find_collectible_for_item(rolledBounty, rolledCollectible);
            // The tile prices whatever it rolls: only its placeholder item is withheld, never
            // its price. A tile the character cannot pay for grants nothing.
            (void)grant_item_definition(message, rolledCollectible, rolledBounty, price, outcome);
        }
        return;
    }
    // Prepared in place; a row that is not an exchange gives the payload back before the grant.
    auto* exchange = emplace_mutation<state::PendingProfileItemAcquisition>(outcome);
    if (exchange == nullptr) {
        report_purchase(opcode, "fail", "storage", vendorIndex, rowIndex, itemDefinitionIndex);
        return;
    }
    if (exchange_vendor_row(vendorIndex, rowIndex, *exchange)) {
        report_purchase(opcode, "ok", "exchange", vendorIndex, rowIndex, itemDefinitionIndex);
        if (!exchange->prepared) {
            clear_mutation(outcome);
        }
        return;
    }
    clear_mutation(outcome);
    // A placeholder row grants what it stands for; the placeholder itself never draws.
    std::uint16_t granted = itemDefinitionIndex;
    std::uint16_t substituteIndex = kUnavailableDefinitionIndex;
    switch (substitute_for_item(granted, substituteIndex)) {
    case Substitution::replaced:
        granted = substituteIndex;
        break;
    case Substitution::broken:
        report_purchase(opcode, "fail", "substitute_missing", vendorIndex, rowIndex, granted);
        return;
    case Substitution::none:
        break;
    }
    std::uint16_t collectibleIndex = state::build_data::collectibles::kNoCollectibleIndex;
    const bool collected = find_collectible_for_item(granted, collectibleIndex);
    report_purchase(opcode,
                    "ok",
                    collected ? "resolved" : "resolved_no_collectible",
                    vendorIndex,
                    rowIndex,
                    granted);
    (void)grant_item_definition(message, collectibleIndex, granted, price, outcome);
}

/**
 * Prepares one opcode-904 quest acquire.
 * A quest names a vendor row exactly as a purchase does and takes the same grant path.
 */
void acquire_quest(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace quest = middleware::web_service::messages::opcode904;
    quest::Request request{};
    if (!quest::parse_request(message, request)) {
        report_purchase(quest::kOpcode, "fail", "payload", -1, -1, kUnavailableDefinitionIndex);
        return;
    }
    // The 32-bit field is the sale row; the 16-bit slot is only where the click landed.
    // Refused rather than guessed, because indexing sale rows by slot grants the wrong item.
    if (!request.hasSaleIndex) {
        report_purchase(quest::kOpcode,
                        "fail",
                        "sale_field_missing",
                        request.vendorIndex,
                        request.slotIndex,
                        kUnavailableDefinitionIndex);
        return;
    }
    const std::int32_t row = request.saleIndex;
    const char* reason = "unknown";
    // A row of -1 says the tile is not a sale row at all, so the installed array answers it.
    // Reading the slot as a sale row here would grant whatever sits at that row.
    const bool rowless = row < 0;
    // A rowless 904 is an interaction reply, so its slot names the interaction, not a sale row:
    // it has no category and no price.
    state::build_data::vendors::SaleRow sale{};
    sale.categoryIndex = state::build_data::vendors::kAbsentCategoryIndex;
    const bool located =
        rowless ? resolve_rowless_quest(request.vendorIndex, request.slotIndex, sale.itemIndex)
                : resolve_vendor_row(request.vendorIndex, row, sale, reason);
    if (!located) {
        report_purchase(quest::kOpcode,
                        "fail",
                        rowless ? "rowless_unresolved" : reason,
                        request.vendorIndex,
                        row,
                        kUnavailableDefinitionIndex);
        // Nothing was granted, so list what this vendor does offer into the Quests tab.
        if (rowless) {
            report_pursuit_rows(request.vendorIndex);
        }
        return;
    }
    settle_vendor_row(message, quest::kOpcode, request.vendorIndex, row, sale, outcome);
}

/**
 * Prepares one opcode-901 vendor purchase, for any Tower vendor.
 * The sale row names an item-definition index, so this hands over to the Collections grant, and
 * the row's own cost is charged with it: a purchase that cannot pay grants nothing.
 */
void purchase_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace purchase = middleware::web_service::messages::opcode901;
    purchase::Request request{};
    if (!purchase::parse_request(message, request)) {
        report_purchase(purchase::kOpcode, "fail", "payload", -1, -1, kUnavailableDefinitionIndex);
        return;
    }
    const char* reason = "unknown";
    state::build_data::vendors::SaleRow sale{};
    if (!resolve_vendor_row(request.vendorIndex, request.saleIndex, sale, reason)) {
        report_purchase(purchase::kOpcode,
                        "fail",
                        reason,
                        request.vendorIndex,
                        request.saleIndex,
                        kUnavailableDefinitionIndex);
        return;
    }
    settle_vendor_row(
        message, purchase::kOpcode, request.vendorIndex, request.saleIndex, sale, outcome);
}

} // namespace sunrise::server::web_service
