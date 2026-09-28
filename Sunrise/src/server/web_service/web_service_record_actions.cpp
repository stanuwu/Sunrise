/** Triumph, title and season-pass reward actions the web service prepares from one request. */

#include <array>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/crypto/random_bytes.h"
#include "../../middleware/web_service/messages/opcode1801.h"
#include "../../middleware/web_service/messages/opcode1821.h"
#include "../../middleware/web_service/messages/opcode2400.h"
#include "../../state/build_data/runtime.h"
#include "../../state/progression/season_pass_reward_catalog.h"
#include "../../state/runtime/runtime.h"
#include "../../state/unlocks/unlocks_records.h"
#include "internal_actions.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {

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
    if (state::prepare_record_reward_grant(
            std::span(rewards).first(rewardCount), recordIndex, *grant)
        != state::RewardPreparation::prepared) {
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
    std::uint64_t seed = 0;
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(&seed, 1)))) {
        clear_mutation(outcome);
        return fail("random_source");
    }
    const char* reason = nullptr;
    if (!state::prepare_season_pass_reward(request.rewardIndex, seed, *grant, &reason)) {
        clear_mutation(outcome);
        return fail(reason != nullptr ? reason : "reward_preparation");
    }
    // Stage the claim before encoding the reply and account push from the transaction's banks.
    if (!state::claim_season_pass_reward(request.rewardIndex)) {
        clear_mutation(outcome);
        return fail("claim_rejected");
    }
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
