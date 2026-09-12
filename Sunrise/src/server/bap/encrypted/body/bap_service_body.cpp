#include <array>
#include <atomic>
#include <cstdio>
#include <limits>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/account_translation/account_translation_response.h"
#include "../../../../middleware/bap/activity_host/activity_host_response.h"
#include "../../../../middleware/bap/certificate.h"
#include "../../../../middleware/bap/client_config/client_config_response.h"
#include "../../../../middleware/bap/family_subscription.h"
#include "../../../../middleware/bap/family_unsubscription.h"
#include "../../../../middleware/bap/user_message/user_message_response.h"
#include "../../../../middleware/encoding/byte_order.h"
#include "../../../../middleware/web_service/messages/opcode505/opcode505_codec.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../gameplay/group/group_host_sessions.h"
#include "../../../web_service/opcode_routes.h"
#include "../../../web_service/web_service_runtime.h"
#include "../activity_host_manager/activity_host_manager_route.h"
#include "../activity_message/activity_message_route.h"
#include "../internal.h"
#include "../matchmaking/matchmaking_route.h"
#include "../queuez/queuez_state_validation.h"
#include "state/investment/store_internal.h"

namespace sunrise::server::bap::encrypted::body {
namespace {

/** Process-wide identity paired with the single account SOID. */
std::atomic<std::uint64_t> g_translatedIdentity{0};

/** Replaces an optimistic Web Service reply when its deferred transaction cannot be staged. */
[[nodiscard]] bool refuse_web_action(const middleware::web_service::Message& message,
                                     std::span<std::byte> output,
                                     std::size_t& written) noexcept {
    // The refusal is the same record as the success, so it takes the opcode's own shape.
    middleware::web_service::ResponseShape shape{};
    sunrise::server::web_service::resolve_response_shape(message.opcode, shape);
    middleware::web_service::StatusResponse status{};
    status.code = 1;
    return middleware::web_service::encode_response(message, shape, status, output, written);
}

/** Accepts only the first account-translation identity and its retries. */
[[nodiscard]] bool pairs_identity(std::uint64_t identity) noexcept {
    std::uint64_t claimed = 0;
    // A repeat of the same identity still pairs: the peer re-asks until the flag sticks.
    return g_translatedIdentity.compare_exchange_strong(
               claimed, identity, std::memory_order_relaxed)
           || claimed == identity;
}

} // namespace

/**
 * Processes the body for one authenticated service route.
 * @param route Service route data found earlier.
 * @param queuezState Queuez versions and residents set up by this BAP peer.
 * @param activity Exact ActivityClient generation owned by this BAP session.
 * @param matchmakingContext State-owned logical context for this BAP session.
 * @param requestBody Borrowed decrypted request body.
 * @param output Caller-owned response-body storage.
 * @param written Receives encoded body bytes.
 * @param outcome Receives one validated transport action or deferred State transaction.
 * @return True when the chosen body codec succeeds.
 */
bool process(const ServiceRoute& route,
             const queuez::SessionState& queuezState,
             const ActivityClientBinding& activity,
             const RosterDecodeMap& rosterDecode,
             state::matchmaking::ContextHandle matchmakingContext,
             std::span<const std::byte> requestBody,
             std::span<std::byte> output,
             std::size_t& written,
             ServiceOutcome& outcome,
             std::span<const state::account::inventory::PresentedItemRow> presentation) noexcept {
    outcome = {};
    switch (route.bodyCodec) {
    case BodyCodec::empty:
        written = 0;
        return true;
    case BodyCodec::accountTranslationResponse: {
        const state::AccountState account = state::account_snapshot();
        std::uint64_t identity = 0;
        server::gameplay::group::HostSessionBinding host{};
        const bool parsed =
            middleware::bap::account_translation::request_identity(requestBody, identity);
        // The primary host asks with identity zero and receives the live region session. Remote
        // activity rows ask by activity-session id. Only other nonzero identities pair to account.
        const std::uint64_t liveSession =
            state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
        const bool zeroHandle =
            parsed && identity == 0 && liveSession != state::activity::kAbsentSessionId;
        const bool logicalHost =
            parsed && identity != 0
            && server::gameplay::group::host_session_for_activity(identity, host)
            && host.target.sessionId == identity;
        const bool accountIdentity =
            parsed && identity != 0 && !logicalHost && pairs_identity(identity);
        const std::uint64_t soid = zeroHandle        ? liveSession
                                   : logicalHost     ? host.target.sessionId
                                   : accountIdentity ? account.primarySoid
                                                     : 0;
        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=queuez stage=translate result=%s identity=0x%016llX "
                                        "soid=0x%016llX",
                                        zeroHandle        ? "paired kind=zero_handle"
                                        : logicalHost     ? "paired kind=logical_host"
                                        : accountIdentity ? "paired kind=account"
                                                          : "unpaired",
                                        static_cast<unsigned long long>(identity),
                                        static_cast<unsigned long long>(soid));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return middleware::bap::account_translation::encode_response(
            requestBody, soid, output, written);
    }
    case BodyCodec::activityHostManagerResponse: {
        auto* allocation = emplace_transaction<state::activity::PendingAllocation>(outcome);
        if (allocation == nullptr) {
            return false;
        }
        bool hasAllocation = false;
        const bool encoded = activity_host_manager::encode_response(
            requestBody, output, written, *allocation, hasAllocation);
        if (!encoded || !hasAllocation) {
            clear_transaction(outcome);
        }
        return encoded;
    }
    case BodyCodec::activityMessageRequest: {
        written = 0;
        auto* plan = emplace_transaction<activity_message::ActivityPlan>(outcome);
        if (plan == nullptr) {
            return false;
        }
        bool hasTransaction = false;
        const bool processed =
            activity_message::process(activity, rosterDecode, requestBody, *plan, hasTransaction);
        if (!processed || !hasTransaction) {
            clear_transaction(outcome);
        }
        return processed;
    }
    case BodyCodec::activityHostResponse: {
        const state::SignOnState& signOn = state::sign_on();
        return middleware::bap::activity_host::encode_response(
            requestBody, signOn.relayAddress, signOn.relayPort, output, written);
    }
    case BodyCodec::clientConfigResponse:
        return middleware::bap::client_config::encode_minimal_response(output, written);
    case BodyCodec::familySubscription: {
        written = 0;
        outcome.hasSubscription =
            middleware::bap::family_subscription::parse(requestBody, outcome.subscription);
        return outcome.hasSubscription;
    }
    case BodyCodec::familyUnsubscription: {
        written = 0;
        outcome.hasUnsubscription =
            middleware::bap::family_unsubscription::parse(requestBody, outcome.unsubscription);
        return outcome.hasUnsubscription;
    }
    case BodyCodec::matchmakingResponse: {
        auto* mutation = emplace_transaction<state::matchmaking::PendingMutation>(outcome);
        if (mutation == nullptr) {
            return false;
        }
        bool hasMutation = false;
        state::PendingCurrentActivity currentActivity{};
        const bool encoded = matchmaking::encode_response(matchmakingContext,
                                                          requestBody,
                                                          output,
                                                          written,
                                                          *mutation,
                                                          hasMutation,
                                                          currentActivity);
        if (!hasMutation) {
            clear_transaction(outcome);
        }
        if (encoded && !hasMutation && currentActivity.prepared) {
            auto* transaction = emplace_transaction<CurrentActivityTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_current_activity_character(
                    queuezState, currentActivity.characterSoid, transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=current_activity stage=queuez_preflight result=fail");
                clear_transaction(outcome);
            } else {
                transaction->pending = currentActivity;
            }
        }
        return encoded;
    }
    case BodyCodec::steamCertificate:
        return middleware::bap::certificate::encode_response(requestBody, output, written);
    case BodyCodec::userMessageResponse:
        return middleware::bap::user_message::encode_minimal_response(output, written);
    case BodyCodec::webService: {
        middleware::web_service::Message message;
        if (middleware::web_service::parse_request(requestBody, message)
            && message.opcode == middleware::web_service::messages::opcode505::kOpcode) {
            auto* changeCharacter = emplace_transaction<queuez::ChangeCharacter>(outcome);
            if (!middleware::web_service::messages::opcode505::parse_request(message)
                || changeCharacter == nullptr
                || !queuez::stage_change_character(queuezState, *changeCharacter)
                || !middleware::web_service::messages::opcode505::encode_response(
                    message, changeCharacter->after.family4Version, output, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws505 stage=change result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            outcome.hasChangeCharacter = true;
            return true;
        }
        state::investment::store::Transaction investmentTransaction;
        if (!investmentTransaction.ready()) {
            return false;
        }
        web_service::Outcome webOutcome;
        if (!sunrise::server::web_service::consume(
                requestBody, output, written, webOutcome, presentation)) {
            return false;
        }
        if (webOutcome.hasTitleEquip) {
            // Promise the revision that refreshes the Seals widget's action binding.
            if (queuezState.family4Version != (std::numeric_limits<std::int32_t>::max)()) {
                middleware::web_service::StatusResponse status{};
                status.value = queuezState.family4Version + 1;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=title_equip stage=response result=fail");
                    sunrise::server::bap::arm_account_resync_everywhere();
                    return false;
                }
            }
            sunrise::server::bap::arm_account_resync_everywhere();
        }
        outcome.hasSubscription = webOutcome.hasSubscription;
        outcome.hasRecordClaim = webOutcome.hasRecordClaim;
        outcome.hasArtifactReset = webOutcome.hasArtifactReset;
        outcome.artifactReset = webOutcome.artifactReset;
        outcome.subscription = webOutcome.subscription;
        const auto* equipmentSwap =
            web_service::mutation_if<state::PendingEquipmentSwap>(webOutcome);
        const auto* subclassSelection =
            web_service::mutation_if<state::PendingSubclassSelection>(webOutcome);
        const auto* socketPlug = web_service::mutation_if<state::PendingSocketPlug>(webOutcome);
        const auto* itemState = web_service::mutation_if<state::PendingItemState>(webOutcome);
        const auto* artifactPurchase =
            web_service::mutation_if<state::PendingArtifactPurchase>(webOutcome);
        const auto* itemAcquisition =
            web_service::mutation_if<state::PendingItemAcquisition>(webOutcome);
        const auto* profileItemAcquisition =
            web_service::mutation_if<state::PendingProfileItemAcquisition>(webOutcome);
        const auto* itemDismantle =
            web_service::mutation_if<state::PendingItemDismantle>(webOutcome);
        const auto* recordRewardGrant =
            web_service::mutation_if<state::PendingRecordRewardGrant>(webOutcome);
        auto* seasonPassReward =
            web_service::mutation_if<state::PendingSeasonPassReward>(webOutcome);
        if (webOutcome.hasArtifactReset) {
            if (queuezState.family4Version == (std::numeric_limits<std::int32_t>::max)()) {
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = queuezState.family4Version + 1;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPairWithBool,
                    status,
                    output,
                    written)) {
                return false;
            }
        }
        if (artifactPurchase != nullptr) {
            auto* transaction = emplace_transaction<ArtifactPurchaseTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_equipment_swap(
                    queuezState, artifactPurchase->characterSoid, transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws901 stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            status.trailingBool = true;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPairWithBool,
                    status,
                    output,
                    written)) {
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingArtifactPurchase>(webOutcome);
        }
        const auto* settingsUpdate =
            web_service::mutation_if<state::PendingSettingsUpdate>(webOutcome);
        if (settingsUpdate != nullptr) {
            // WS-701 promises no immediate QueueZ after-image, so State alone is delayed.
            if (emplace_transaction<state::PendingSettingsUpdate>(outcome, *settingsUpdate)
                == nullptr) {
                return refuse_web_action(message, output, written);
            }
        }
        if (equipmentSwap != nullptr) {
            // Promise the Family-4 revision carrying this optimistic equip.
            auto* transaction = emplace_transaction<EquipmentSwapTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_equipment_swap(
                    queuezState, equipmentSwap->characterSoid, transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws403 stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws403 stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingEquipmentSwap>(webOutcome);
        }
        if (subclassSelection != nullptr) {
            // Promise the Family-4 revision carrying the subclass socket change.
            auto* transaction = emplace_transaction<SubclassSelectionTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_subclass_selection(queuezState,
                                                     subclassSelection->accountSoid,
                                                     subclassSelection->characterSoid,
                                                     subclassSelection->subclassInstanceSoid,
                                                     transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=subclass_select stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=subclass_select stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingSubclassSelection>(webOutcome);
        }
        if (socketPlug != nullptr) {
            // Promise the Family-4 revision carrying the changed item instance.
            auto* transaction = emplace_transaction<SocketPlugTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_socket_plug(queuezState,
                                              socketPlug->accountSoid,
                                              socketPlug->characterSoid,
                                              socketPlug->targetInstanceSoid,
                                              socketPlug->profileChanged,
                                              transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=socket_plug stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=socket_plug stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending = web_service::take_mutation<state::PendingSocketPlug>(webOutcome);
        }
        if (itemState != nullptr) {
            // Promise the Family-4 revision carrying the changed row flags.
            auto* transaction = emplace_transaction<ItemStateTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_equipment_swap(
                    queuezState, itemState->characterSoid, transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=item_state stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=item_state stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending = web_service::take_mutation<state::PendingItemState>(webOutcome);
        }
        if (itemAcquisition != nullptr) {
            // Promise the revision adding both the inventory row and resident instance.
            auto* transaction = emplace_transaction<ItemAcquisitionTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_item_acquisition(queuezState,
                                                   itemAcquisition->accountSoid,
                                                   itemAcquisition->characterSoid,
                                                   itemAcquisition->acquiredInstanceSoid,
                                                   itemAcquisition->updates_account(),
                                                   transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=acquire stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=acquire stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingItemAcquisition>(webOutcome);
        }
        if (profileItemAcquisition != nullptr) {
            // Actionable profile stacks may add a resident in the same revision.
            auto* transaction = emplace_transaction<ProfileItemAcquisitionTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_profile_item_acquisition(
                    queuezState,
                    profileItemAcquisition->accountSoid,
                    profileItemAcquisition->acquiredInstanceSoid,
                    profileItemAcquisition->actionSource,
                    profileItemAcquisition->appended,
                    transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=profile_acquire stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=profile_acquire stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingProfileItemAcquisition>(webOutcome);
        }
        if (itemDismantle != nullptr) {
            // Promise the revision carrying the character update and resident release.
            auto* transaction = emplace_transaction<ItemDismantleTransaction>(outcome);
            if (transaction == nullptr
                || !queuez::stage_item_dismantle(queuezState,
                                                 itemDismantle->accountSoid,
                                                 itemDismantle->characterSoid,
                                                 itemDismantle->dismantledInstanceSoid,
                                                 itemDismantle->profileChanged,
                                                 transaction->update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=dismantle stage=queuez_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=dismantle stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingItemDismantle>(webOutcome);
        }
        if (recordRewardGrant != nullptr) {
            auto* transaction = emplace_transaction<RecordRewardGrantTransaction>(outcome);
            std::array<std::uint64_t, state::kRecordRewardGrantCapacity> residents{};
            std::size_t residentCount = 0;
            for (std::size_t index = 0; index < recordRewardGrant->rewardCount; ++index) {
                const auto& reward = recordRewardGrant->rewards[index];
                if (reward.kind == state::RecordRewardKind::characterInstance
                    || reward.appendedProfileResident) {
                    residents[residentCount++] = reward.instanceSoid;
                }
            }
            const bool staged =
                transaction != nullptr
                && queuez::stage_record_reward_grant(queuezState,
                                                     recordRewardGrant->accountSoid,
                                                     recordRewardGrant->characterSoid,
                                                     std::span(residents).first(residentCount),
                                                     transaction->update);
            if (!staged) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws1801 stage=reward_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = transaction->update.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws1801 stage=reward_response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingRecordRewardGrant>(webOutcome);
        }
        if (seasonPassReward != nullptr) {
            auto* transaction = emplace_transaction<SeasonPassRewardTransaction>(outcome);
            if (transaction == nullptr) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws2400 stage=reward_preflight result=fail");
                return refuse_web_action(message, output, written);
            }
            bool staged = false;
            std::int32_t stagedVersion = 0;
            if (const auto* itemGrant =
                    std::get_if<state::PendingItemAcquisition>(&seasonPassReward->grant)) {
                auto& update = transaction->update.emplace<queuez::ItemAcquisition>();
                staged = queuez::stage_item_acquisition(queuezState,
                                                        itemGrant->accountSoid,
                                                        itemGrant->characterSoid,
                                                        itemGrant->acquiredInstanceSoid,
                                                        true,
                                                        update);
                stagedVersion = update.after.family4Version;
            } else if (const auto* profileGrant = std::get_if<state::PendingProfileItemAcquisition>(
                           &seasonPassReward->grant)) {
                auto& update = transaction->update.emplace<queuez::ProfileItemAcquisition>();
                staged = queuez::stage_profile_item_acquisition(queuezState,
                                                                profileGrant->accountSoid,
                                                                profileGrant->acquiredInstanceSoid,
                                                                profileGrant->actionSource,
                                                                profileGrant->appended,
                                                                update);
                stagedVersion = update.after.family4Version;
            } else if (const auto* bundle =
                           std::get_if<state::PendingDirectItemBundle>(&seasonPassReward->grant)) {
                staged = queuez::stage_direct_item_bundle(queuezState,
                                                          bundle->accountSoid,
                                                          bundle->characterSoid,
                                                          bundle->firstInstanceSoid,
                                                          bundle->itemCount,
                                                          stagedVersion);
            } else if (const auto* resources =
                           std::get_if<state::PendingRecordRewardGrant>(&seasonPassReward->grant)) {
                auto& update = transaction->update.emplace<queuez::RecordRewardGrant>();
                staged = queuez::stage_record_reward_grant(
                    queuezState, resources->accountSoid, resources->characterSoid, {}, update);
                stagedVersion = update.after.family4Version;
            }
            if (!staged) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws2400 stage=reward_preflight result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            middleware::web_service::StatusResponse status{};
            status.value = stagedVersion;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws2400 stage=response result=fail");
                clear_transaction(outcome);
                return refuse_web_action(message, output, written);
            }
            transaction->pending =
                web_service::take_mutation<state::PendingSeasonPassReward>(webOutcome);
        }
        if (webOutcome.hasRecordClaim && !has_transaction(outcome)
            && queuezState.family4Version != (std::numeric_limits<std::int32_t>::max)()) {
            // A plain claim is made authoritative by the following account resync.
            middleware::web_service::StatusResponse status{};
            status.value = queuezState.family4Version + 1;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws1801 stage=response result=fail");
                sunrise::server::bap::arm_account_resync_everywhere();
                return false;
            }
        }
        if (webOutcome.hasSelectedCharacter && webOutcome.selectedCharacterChanged) {
            auto* selectCharacter = emplace_transaction<queuez::SelectCharacter>(outcome);
            if (selectCharacter != nullptr
                && queuez::stage_select_character(
                    queuezState, webOutcome.selectedCharacterSoid, *selectCharacter)) {
                outcome.hasSelectCharacter = true;
            } else {
                clear_transaction(outcome);
                sunrise::server::bap::arm_account_resync_everywhere();
            }
        }
        return investmentTransaction.commit();
    }
    }
    written = 0;
    return false;
}

} // namespace sunrise::server::bap::encrypted::body
