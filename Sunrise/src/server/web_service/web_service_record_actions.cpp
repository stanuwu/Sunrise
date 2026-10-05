/** Triumph, title and season-pass reward actions the web service prepares from one request. */

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/crypto/random_bytes.h"
#include "../../middleware/encoding/byte_order.h"
#include "../../middleware/web_service/messages/opcode1801.h"
#include "../../middleware/web_service/messages/opcode1821.h"
#include "../../middleware/web_service/messages/opcode2400.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/items/item_catalog.h"
#include "../../state/build_data/runtime.h"
#include "../../state/progression/season_pass_reward_catalog.h"
#include "../../state/runtime/runtime.h"
#include "../../state/unlocks/unlocks_records.h"
#include "internal_actions.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {

namespace {

/** Prepares one rank-one class package without changing account State. */
[[nodiscard]] bool
prepare_premium_class_package(const state::build_data::season_pass::Package& package,
                              state::PendingDirectItemBundle& mutation) noexcept {
    std::array<std::uint16_t, state::build_data::season_pass::kPackageItemCapacity> itemIndices{};
    if (package.itemCount == 0 || package.itemCount > itemIndices.size()) {
        return false;
    }
    for (std::size_t index = 0; index < package.itemCount; ++index) {
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(package.items[index], definition)) {
            return false;
        }
        itemIndices[index] = definition.definitionIndex;
    }
    return state::prepare_direct_item_bundle(
        package.definitionHash, std::span(itemIndices).first(package.itemCount), mutation);
}

/** Expands the manifest-authored destination package into all nine material stacks. */
[[nodiscard]] bool
prepare_destination_resource_bundle(state::PendingSeasonPassReward& grant) noexcept {
    namespace pass = state::progression::season_pass;
    std::array<state::DirectRecordReward, pass::kDestinationResourceHashes.size()> rewards{};
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(pass::kDestinationResourceHashes[index],
                                                          definition)) {
            return false;
        }
        rewards[index] = {definition.definitionIndex, pass::kDestinationResourceQuantity};
    }
    auto& mutation = grant.grant.emplace<state::PendingRecordRewardGrant>();
    return state::prepare_record_reward_grant(rewards, {}, mutation);
}

/** Chooses one installed weapon or selected-class armour item from an auto-decrypting engram. */
template <std::size_t Size>
[[nodiscard]] std::span<const std::uint32_t>
class_armour_pool(state::CharacterClass characterClass,
                  const std::array<std::uint32_t, Size>& titan,
                  const std::array<std::uint32_t, Size>& hunter,
                  const std::array<std::uint32_t, Size>& warlock) noexcept {
    switch (characterClass) {
    case state::CharacterClass::hunter:
        return hunter;
    case state::CharacterClass::warlock:
        return warlock;
    case state::CharacterClass::titan:
    default:
        return titan;
    }
}

/**
 * Picks one random installed item from the engram's weapon and selected-class armour pools.
 * @param itemIndex Receives the picked item definition index; unchanged on failure.
 * @return False when no character is selected, the engram is unknown, or no pool item is installed.
 */
[[nodiscard]] bool choose_engram_reward(std::uint32_t engramHash,
                                        std::uint16_t& itemIndex) noexcept {
    namespace pass = state::progression::season_pass;
    std::span<const std::uint32_t> weapons;
    std::span<const std::uint32_t> armour;
    const state::AccountState account = state::bound_account_snapshot();
    const state::CharacterState* character = nullptr;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            character = &account.characters[index];
            break;
        }
    }
    if (character == nullptr) {
        return false;
    }

    if (engramHash == pass::kLegendaryEngramHash) {
        weapons = pass::kLegendaryEngramWeapons;
        armour = class_armour_pool(character->characterClass,
                                   pass::kLegendaryTitanArmour,
                                   pass::kLegendaryHunterArmour,
                                   pass::kLegendaryWarlockArmour);
    } else if (engramHash == pass::kExoticEngramHash) {
        weapons = pass::kExoticEngramWeapons;
        armour = class_armour_pool(character->characterClass,
                                   pass::kExoticTitanArmour,
                                   pass::kExoticHunterArmour,
                                   pass::kExoticWarlockArmour);
    } else {
        return false;
    }

    std::array<std::byte, sizeof(std::uint32_t)> randomBytes{};
    if (!middleware::crypto::random::fill(randomBytes)) {
        return false;
    }
    const std::size_t count = weapons.size() + armour.size();
    const std::size_t first = middleware::encoding::read_u32_le(randomBytes) % count;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t selected = (first + offset) % count;
        const std::uint32_t hash =
            selected < weapons.size() ? weapons[selected] : armour[selected - weapons.size()];
        state::build_data::items::Definition definition{};
        if (state::build_data::find_item_definition_hash(hash, definition)) {
            itemIndex = definition.definitionIndex;
            return true;
        }
    }
    return false;
}

/** Stat plugs occupy the four ordinary socket lanes after the six the armour piece itself uses. */
constexpr std::size_t kSeasonArmourStatLaneBase = 6;
/** One roll names one lane of each of the four armour stats. */
constexpr std::size_t kSeasonArmourStatLaneCount = 4;

struct SeasonArmourRoll {
    std::uint16_t rewardIndex{};
    std::uint32_t itemHash{};
    std::array<std::uint32_t, kSeasonArmourStatLaneCount> statPlugs{};
};

/**
 * Exact rolls shown by the free, early premium, and high-stat pass tiles.
 * TODO: extract; the pass reward row names the item, and no package field names its stat plugs.
 */
constexpr std::array<SeasonArmourRoll, 36> kSeasonArmourRolls{{
    // Hunter
    {29, 3097544525U, {1232924472U, 3898228147U, 1177742666U, 1322519294U}},
    {106, 3097544525U, {2191854732U, 3198072315U, 1322519294U, 597281635U}},
    {139, 3097544525U, {3072193746U, 772656086U, 2692705068U, 1300719726U}},
    {3, 3750210364U, {1232924472U, 3898228147U, 1177742666U, 757147114U}},
    {78, 3750210364U, {2120002858U, 1526511067U, 1277668557U, 3014984195U}},
    {111, 3750210364U, {2600679334U, 3955940376U, 3026009912U, 115819390U}},
    {24, 2930001572U, {1232924472U, 2518640481U, 1177742666U, 4237052128U}},
    {97, 2930001572U, {2191854732U, 2440285137U, 1322519294U, 597281635U}},
    {134, 2930001572U, {4286256569U, 2148570570U, 594234536U, 2664898188U}},
    {10, 3136019014U, {2191854732U, 2236091344U, 1177742666U, 2596709069U}},
    {83, 3136019014U, {3407231789U, 1798836563U, 1322519294U, 3203727595U}},
    {120, 3136019014U, {3346214146U, 2399358832U, 594234536U, 2701881372U}},
    // Titan
    {30, 1214477175U, {1232924472U, 3898228147U, 1177742666U, 1322519294U}},
    {107, 1214477175U, {479726201U, 176934377U, 1322519294U, 597281635U}},
    {140, 1214477175U, {2868325782U, 2387165386U, 2692705068U, 1300719726U}},
    {4, 287888126U, {1232924472U, 3898228147U, 1177742666U, 757147114U}},
    {79, 287888126U, {2120002858U, 1369119565U, 1277668557U, 3014984195U}},
    {112, 287888126U, {2148570570U, 2148570570U, 3026009912U, 115819390U}},
    {25, 1585947570U, {1232924472U, 2518640481U, 1177742666U, 4237052128U}},
    {98, 1585947570U, {1232924472U, 1533918171U, 1322519294U, 597281635U}},
    {135, 1585947570U, {2868325782U, 1087628722U, 594234536U, 2664898188U}},
    {11, 1131831128U, {2191854732U, 2236091344U, 1177742666U, 2596709069U}},
    {84, 1131831128U, {1232924472U, 4002613733U, 1322519294U, 3203727595U}},
    {121, 1131831128U, {2387165386U, 2148570570U, 594234536U, 2701881372U}},
    // Warlock
    {31, 1173249516U, {1232924472U, 3898228147U, 1177742666U, 1322519294U}},
    {108, 1173249516U, {2191854732U, 2433764085U, 1322519294U, 597281635U}},
    {141, 1173249516U, {2868325782U, 3072193746U, 2692705068U, 1300719726U}},
    {5, 327547301U, {1232924472U, 3898228147U, 1177742666U, 757147114U}},
    {80, 327547301U, {1232924472U, 3554741641U, 1277668557U, 3014984195U}},
    {113, 327547301U, {4248662490U, 1281324436U, 3026009912U, 115819390U}},
    {26, 119457531U, {1232924472U, 2518640481U, 1177742666U, 4237052128U}},
    {99, 119457531U, {1232924472U, 1858911761U, 1322519294U, 597281635U}},
    {136, 119457531U, {3198618292U, 643846500U, 594234536U, 2664898188U}},
    {12, 674876967U, {2191854732U, 2236091344U, 1177742666U, 2596709069U}},
    {85, 674876967U, {2576224072U, 1534361879U, 1322519294U, 3203727595U}},
    {122, 674876967U, {643846500U, 4248662490U, 594234536U, 2701881372U}},
}};

/** Replaces native stat plugs with the fixed roll represented by one known pass tile. */
[[nodiscard]] bool apply_season_armour_roll(std::uint16_t rewardIndex,
                                            state::PendingItemAcquisition& mutation) noexcept {
    const auto roll = std::find_if(kSeasonArmourRolls.begin(),
                                   kSeasonArmourRolls.end(),
                                   [rewardIndex](const SeasonArmourRoll& candidate) {
                                       return candidate.rewardIndex == rewardIndex;
                                   });
    if (roll == kSeasonArmourRolls.end()) {
        return true;
    }
    if (mutation.inventoryIndex >= mutation.afterCharacter.inventory.count) {
        return false;
    }
    auto& acquired = mutation.afterCharacter.inventory.values[mutation.inventoryIndex];
    state::build_data::items::Definition item{};
    state::build_data::items::details::Definition detail{};
    if (acquired.definitionHash != mutation.acquiredDefinitionHash
        || acquired.definitionHash != roll->itemHash
        || !state::build_data::find_item_definition_hash(acquired.definitionHash, item)
        || !state::build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex
        || detail.ordinarySocketCount < kSeasonArmourStatLaneBase + kSeasonArmourStatLaneCount
        || detail.ordinarySocketCount > acquired.sockets.plugs.size()) {
        return false;
    }

    state::account::inventory::Sockets sockets{};
    sockets.policy = state::account::inventory::SocketPolicy::authored;
    sockets.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < sockets.plugCount; ++lane) {
        const std::uint16_t plugIndex = detail.initialPlugIndices[lane];
        if (plugIndex == state::build_data::items::details::kUnavailableItemIndex) {
            continue;
        }
        state::build_data::items::Definition plug{};
        if (!state::build_data::find_item_definition_index(plugIndex, plug)) {
            return false;
        }
        sockets.plugs[lane] = plug.definitionHash;
    }
    for (std::size_t index = 0; index < roll->statPlugs.size(); ++index) {
        sockets.plugs[kSeasonArmourStatLaneBase + index] = roll->statPlugs[index];
    }
    acquired.sockets = sockets;
    return state::account::inventory::valid(acquired.sockets);
}

} // namespace

/** Reports an opcode-1801 claim failure. */
void report_record_claim(const middleware::web_service::Message& message,
                         std::string_view reason,
                         std::uint32_t recordIndex,
                         std::uint32_t completionFlagIndex,
                         std::uint32_t scoreValue) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1801 stage=claim result=fail reason=%.*s transaction=%u payload_bytes=%zu "
        "record_index=%u completion_flag_index=%u score=%u total_score=%u",
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        recordIndex,
        completionFlagIndex,
        scoreValue,
        state::unlocks::records::score());
    write_warning(line, count);
}

/** Reports a record-reward preparation failure. */
void report_record_reward(const middleware::web_service::Message& message,
                          std::string_view reason,
                          std::uint32_t recordIndex,
                          std::uint32_t itemIndex,
                          std::int32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=ws1801 stage=reward result=fail reason=%.*s transaction=%u "
                                    "record_index=%u item_index=%u quantity=%d",
                                    static_cast<int>(reason.size()),
                                    reason.data(),
                                    static_cast<unsigned>(message.transactionId),
                                    recordIndex,
                                    itemIndex,
                                    quantity);
    write_warning(line, count);
}

/** Prepares one direct Season-pass item, returning a failure reason or null. */
[[nodiscard]] const char* prepare_direct_reward(std::uint16_t itemIndex,
                                                std::int32_t quantity,
                                                state::PendingSeasonPassReward& grant) noexcept {
    if (quantity <= 0) {
        return "quantity";
    }
    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemIndex, definition)) {
        return "item_definition";
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemIndex, detail)
        || detail.definitionIndex != itemIndex || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        return "item_detail_or_bucket";
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            return "profile_item_instanced";
        }
        auto& mutation = grant.grant.emplace<state::PendingProfileItemAcquisition>();
        return state::prepare_profile_item_acquisition_for_item(itemIndex, quantity, mutation)
                   ? nullptr
                   : "profile_state";
    }
    if (bucket.arraySelector == bucket_domain::ArraySelector::character) {
        if (quantity != 1) {
            return "character_item_quantity";
        }
        auto& mutation = grant.grant.emplace<state::PendingItemAcquisition>();
        if (!state::prepare_item_acquisition_for_item(itemIndex, mutation)) {
            return "state";
        }
        mutation.profileChanged = false;
        return nullptr;
    }
    return "unsupported_inventory_array";
}

/** Whether a claimed record owes a reward, and whether that reward could be prepared. */
enum class RecordRewardPreparation : std::uint8_t {
    /** The record grants nothing, so only the claim is written. */
    absent,
    /** The reward is staged and its commit revokes the claim if it is refused. */
    prepared,
    /** The record owes a reward that could not be prepared; nothing is claimed. */
    failed,
};

/** Prepares the reward rows the record names, naming the record its commit would revoke. */
[[nodiscard]] RecordRewardPreparation
prepare_record_reward(const middleware::web_service::Message& message,
                      std::uint16_t recordIndex,
                      Outcome& outcome) noexcept {
    std::array<state::DirectRecordReward, state::kRecordRewardGrantCapacity> rewards{};
    std::size_t rewardCount = 0;
    std::array<state::build_data::records::Reward,
               state::build_data::records::kRewardPerRecordCapacity>
        extracted{};
    if (!state::build_data::find_record_rewards(recordIndex, extracted, rewardCount)) {
        report_record_reward(message, "reward_table", recordIndex, 0, 0);
        return RecordRewardPreparation::failed;
    }
    if (rewardCount == 0) {
        return RecordRewardPreparation::absent;
    }
    for (std::size_t index = 0; index < rewardCount; ++index) {
        rewards[index] = {extracted[index].itemIndex, extracted[index].quantity};
    }

    auto* grant = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
    if (grant == nullptr) {
        report_record_reward(
            message, "storage", recordIndex, rewards[0].itemDefinitionIndex, rewards[0].quantity);
        return RecordRewardPreparation::failed;
    }
    if (!state::prepare_record_reward_grant(
            std::span(rewards).first(rewardCount), recordIndex, *grant)) {
        clear_mutation(outcome);
        report_record_reward(
            message, "state", recordIndex, rewards[0].itemDefinitionIndex, rewards[0].quantity);
        return RecordRewardPreparation::failed;
    }
    return RecordRewardPreparation::prepared;
}

/** Claims one exact reward-array row from the active Season of Arrivals pass. */
void claim_season_pass_reward(const middleware::web_service::Message& message,
                              Outcome& outcome) noexcept {
    namespace pass = state::progression::season_pass;
    middleware::web_service::messages::opcode2400::Request request{};
    state::build_data::season_pass::Reward reward{};
    state::build_data::items::Definition item{};
    const std::uint16_t rank = state::seasonal_rank();
    const auto fail = [&](std::string_view reason) noexcept {
        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws2400 stage=claim result=fail reason=%.*s transaction=%u progression=%u "
            "reward=%u rank=%u item_hash=0x%08X item_index=%u",
            static_cast<int>(reason.size()),
            reason.data(),
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned>(request.progressionIndex),
            static_cast<unsigned>(request.rewardIndex),
            static_cast<unsigned>(rank),
            reward.itemHash,
            static_cast<unsigned>(item.definitionIndex));
        write_warning(line, count);
    };

    if (!middleware::web_service::messages::opcode2400::parse_request(message, request)) {
        return fail("payload_bits");
    }
    if (request.progressionIndex != pass::kProgressionDefinitionIndex) {
        return fail("progression");
    }
    if (!state::build_data::find_season_pass_reward(request.rewardIndex, reward)) {
        return fail("reward_index");
    }
    if (reward.requiredRank > rank) {
        return fail("rank");
    }
    if (state::season_pass_reward_claimed(request.rewardIndex)) {
        return fail("already_claimed");
    }
    if (!state::build_data::find_item_definition_hash(reward.itemHash, item)) {
        return fail("item_hash");
    }
    auto* grant = emplace_mutation<state::PendingSeasonPassReward>(outcome);
    if (grant == nullptr) {
        return fail("storage");
    }
    state::build_data::season_pass::Package package{};
    if (state::build_data::find_season_pass_package(reward.itemHash, package)) {
        auto& bundle = grant->grant.emplace<state::PendingDirectItemBundle>();
        if (!prepare_premium_class_package(package, bundle)) {
            clear_mutation(outcome);
            return fail("package_grant");
        }
    } else if (reward.itemHash == pass::kDestinationResourceBundleHash) {
        if (!prepare_destination_resource_bundle(*grant)) {
            clear_mutation(outcome);
            return fail("resource_bundle");
        }
    } else if (reward.itemHash == pass::kLegendaryEngramHash
               || reward.itemHash == pass::kExoticEngramHash) {
        std::uint16_t decryptedItemIndex = 0;
        if (!choose_engram_reward(reward.itemHash, decryptedItemIndex)) {
            clear_mutation(outcome);
            return fail("engram_pool");
        }
        if (const char* reason = prepare_direct_reward(decryptedItemIndex, 1, *grant)) {
            clear_mutation(outcome);
            return fail(reason);
        }
    } else {
        if (const char* reason = prepare_direct_reward(
                item.definitionIndex, static_cast<std::int32_t>(reward.quantity), *grant)) {
            clear_mutation(outcome);
            return fail(reason);
        }
        if (auto* armour = std::get_if<state::PendingItemAcquisition>(&grant->grant);
            armour != nullptr && !apply_season_armour_roll(request.rewardIndex, *armour)) {
            clear_mutation(outcome);
            return fail("armour_roll");
        }
    }
    // The claim is world state the moment the grant is accepted, so it is written before the
    // reply and the push are staged from the banks.
    if (!state::claim_season_pass_reward(request.rewardIndex)) {
        clear_mutation(outcome);
        return fail("claim_rejected");
    }
    grant->sourceDefinitionHash = reward.itemHash;
    grant->rewardIndex = request.rewardIndex;
    grant->prepared = true;
}

/** Decodes one opcode-1801 Triumphs claim and reports the record it names. */
void claim_record(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace records = state::build_data::records;
    middleware::web_service::messages::opcode1801::Request request{};
    if (!middleware::web_service::messages::opcode1801::parse_request(message, request)) {
        report_record_claim(message, "payload_bits", 0, records::kUnavailableFlagIndex, 0);
        return;
    }
    records::Definition definition{};
    if (!state::build_data::find_record_definition(request.recordIndex, definition)) {
        report_record_claim(
            message, "record_definition", request.recordIndex, records::kUnavailableFlagIndex, 0);
        return;
    }
    if (definition.completionFlagIndex == records::kUnavailableFlagIndex) {
        // Interval Triumphs intentionally carry no completion flag. Their repeated redemption
        // count is projected through the second reserved objective-value slot instead.
        if (state::unlocks::records::claim_interval(request.recordIndex,
                                                    definition.definitionHash)) {
            outcome.hasRecordClaim = true;
            return;
        }
        report_record_claim(message,
                            "interval_or_flag_unavailable",
                            request.recordIndex,
                            records::kUnavailableFlagIndex,
                            definition.scoreValue);
        return;
    }
    if (state::unlocks::records::claimed(definition.completionFlagIndex)) {
        report_record_claim(message,
                            "claim_rejected",
                            request.recordIndex,
                            definition.completionFlagIndex,
                            definition.scoreValue);
        return;
    }

    const RecordRewardPreparation reward =
        prepare_record_reward(message, request.recordIndex, outcome);
    if (reward == RecordRewardPreparation::failed) {
        return;
    }
    // The claim is world state the moment it is accepted, so it is written before the reply and
    // the push are staged from the banks. A refused commit revokes it.
    if (!state::unlocks::records::claim(request.recordIndex)) {
        if (reward == RecordRewardPreparation::prepared) {
            clear_mutation(outcome);
        }
        report_record_claim(message,
                            "claim_rejected",
                            request.recordIndex,
                            definition.completionFlagIndex,
                            definition.scoreValue);
        return;
    }
    outcome.hasRecordClaim = true;
}

/** Reports one refused opcode-1821 title selection with the step that refused it. */
void report_title_equip(const middleware::web_service::Message& message,
                        const char* reason,
                        std::uint16_t recordIndex,
                        const state::build_data::records::Definition& definition,
                        std::uint64_t characterSoid) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=title_equip result=fail reason=%s opcode=%u transaction=%u record=%u "
                      "definition_hash=0x%08X completion_flag=%u character=0x%llX",
                      reason,
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      static_cast<unsigned>(recordIndex),
                      definition.definitionHash,
                      static_cast<unsigned>(definition.completionFlagIndex),
                      static_cast<unsigned long long>(characterSoid));
    write_warning(line, count);
}

/** Equips an earned title record on the selected character. */
void equip_title(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace opcode1821 = middleware::web_service::messages::opcode1821;
    namespace records = state::build_data::records;
    opcode1821::Request request{};
    records::Definition definition{};
    std::uint64_t characterSoid = 0;
    bool changed = false;
    if (!opcode1821::parse_request(message, request)) {
        report_title_equip(message, "payload_bits", 0, definition, characterSoid);
        return;
    }
    const bool unequips = request.recordIndex == opcode1821::kUnequippedRecordIndex;
    if (!unequips) {
        if (!state::build_data::find_record_definition(request.recordIndex, definition)) {
            report_title_equip(
                message, "record_definition", request.recordIndex, definition, characterSoid);
            return;
        }
        if (!definition.hasTitle) {
            report_title_equip(
                message, "not_title", request.recordIndex, definition, characterSoid);
            return;
        }
        if (definition.completionFlagIndex == records::kUnavailableFlagIndex
            || !state::unlocks::records::claimed(definition.completionFlagIndex)) {
            report_title_equip(
                message, "not_claimed", request.recordIndex, definition, characterSoid);
            return;
        }
    }
    const std::uint16_t selected =
        unequips ? state::kUnequippedTitleRecordIndex : request.recordIndex;
    if (!state::set_selected_title(selected, characterSoid, changed)) {
        report_title_equip(
            message, "selected_character", request.recordIndex, definition, characterSoid);
        return;
    }
    outcome.hasTitleEquip = true;
}

} // namespace sunrise::server::web_service
