#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/account_translation/account_translation_response.h"
#include "../../../../middleware/bap/activity_host/activity_host_response.h"
#include "../../../../middleware/bap/certificate.h"
#include "../../../../middleware/bap/client_config/client_config_response.h"
#include "../../../../middleware/bap/family_subscription.h"
#include "../../../../middleware/bap/family_unsubscription.h"
#include "../../../../middleware/bap/user_message/user_message_response.h"
#include "../../../../middleware/encoding/byte_order.h"
#include "../../../../middleware/web_service/messages/opcode501_codec.h"
#include "../../../../middleware/web_service/messages/opcode502.h"
#include "../../../../middleware/web_service/messages/opcode505/opcode505_codec.h"
#include "../../../../state/runtime/character_creation.h"
#include "../../../../state/runtime/character_deletion.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../web_service/web_service_runtime.h"
#include "../activity_host_manager/activity_host_manager_route.h"
#include "../activity_message/activity_message_route.h"
#include "../internal.h"
#include "../matchmaking/matchmaking_route.h"
#include "../queuez/queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::body {
namespace {

constexpr std::size_t kSubscribeReportLimit = 96;
constexpr std::size_t kTranslationIdentityOffset = 4;
constexpr std::size_t kTranslationRequestSize =
    kTranslationIdentityOffset + middleware::encoding::kU64Size;

std::atomic<std::uint64_t> g_translatedIdentity{0};

[[nodiscard]] bool pairs_identity(std::span<const std::byte> requestBody) noexcept {
    if (requestBody.size() < kTranslationRequestSize) {
        return false;
    }
    const std::uint64_t identity = middleware::encoding::read_u64_be(
        requestBody.subspan<kTranslationIdentityOffset, middleware::encoding::kU64Size>());
    if (identity == 0) {
        return false;
    }
    std::uint64_t claimed = 0;
    return g_translatedIdentity.compare_exchange_strong(
               claimed, identity, std::memory_order_relaxed)
           || claimed == identity;
}

/** Encodes a non-mutating create response so a refused request still completes its task. */
[[nodiscard]] bool encode_create_fallback(const middleware::web_service::Message& message,
                                          std::span<std::byte> output,
                                          std::size_t& written) noexcept {
    return middleware::web_service::messages::opcode501::encode_response(
        message,
        state::account::selected_character_soid(state::account_snapshot()),
        output,
        written);
}

} // namespace

bool process(const ServiceRoute& route,
             const queuez::SessionState& queuezState,
             const ActivityClientBinding& activity,
             state::matchmaking::ContextHandle matchmakingContext,
             std::span<const std::byte> requestBody,
             std::span<std::byte> output,
             std::size_t& written,
             ServiceOutcome& outcome) noexcept {
    outcome = {};
    switch (route.bodyCodec) {
    case BodyCodec::empty:
        written = 0;
        return true;
    case BodyCodec::accountTranslationResponse: {
        const state::AccountState account = state::account_snapshot();
        const bool pairs = pairs_identity(requestBody);
        const std::uint64_t soid = pairs ? account.primarySoid : 0;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         pairs ? "ev=queuez stage=translate result=paired"
                               : "ev=queuez stage=translate result=unpaired");
        return middleware::bap::account_translation::encode_response(
            requestBody, soid, output, written);
    }
    case BodyCodec::activityHostManagerResponse: {
        state::activity::PendingAllocation allocation{};
        bool hasAllocation = false;
        const bool encoded = activity_host_manager::encode_response(
            requestBody, output, written, allocation, hasAllocation);
        if (encoded && hasAllocation) {
            outcome.transaction = allocation;
        }
        return encoded;
    }
    case BodyCodec::activityMessageRequest: {
        written = 0;
        activity_message::ActivityPlan plan{};
        bool hasTransaction = false;
        const bool processed =
            activity_message::process(activity, requestBody, plan, hasTransaction);
        if (processed && hasTransaction) {
            outcome.transaction = plan;
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
        std::array<char, kSubscribeReportLimit> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=queuez stage=subscribe result=%s family=%u root=0x%016llX",
                          outcome.hasSubscription ? "ok" : "unreadable",
                          static_cast<unsigned>(outcome.subscription.familyType),
                          static_cast<unsigned long long>(outcome.subscription.familyRootSoid));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return outcome.hasSubscription;
    }
    case BodyCodec::familyUnsubscription: {
        written = 0;
        outcome.hasUnsubscription =
            middleware::bap::family_unsubscription::parse(requestBody, outcome.unsubscription);
        return outcome.hasUnsubscription;
    }
    case BodyCodec::matchmakingResponse: {
        state::matchmaking::PendingMutation mutation{};
        bool hasMutation = false;
        const bool encoded = matchmaking::encode_response(
            matchmakingContext, requestBody, output, written, mutation, hasMutation);
        if (encoded && hasMutation) {
            outcome.transaction = mutation;
        }
        return encoded;
    }
    case BodyCodec::steamCertificate:
        return middleware::bap::certificate::encode_response(requestBody, output, written);
    case BodyCodec::userMessageResponse:
        return middleware::bap::user_message::encode_minimal_response(output, written);
    case BodyCodec::webService: {
        middleware::web_service::Message message;
        const bool parsedMessage = middleware::web_service::parse_request(requestBody, message);
        if (parsedMessage
            && message.opcode == middleware::web_service::messages::opcode501::kOpcode) {
            middleware::web_service::messages::opcode501::DecodedRequest decoded{};
            state::PendingCharacterCreation mutation{};
            state::CharacterCreationResult result = state::CharacterCreationResult::invalid;
            if (middleware::web_service::messages::opcode501::decode_request(message, decoded)) {
                state::NativeCharacterCreation creation{};
                creation.race = static_cast<state::CharacterRace>(decoded.race);
                creation.gender = static_cast<state::CharacterGender>(decoded.gender);
                creation.characterClass = static_cast<state::CharacterClass>(decoded.characterClass);
                creation.presentationHeader = decoded.presentationHeader;
                creation.creationHeader = decoded.creationHeader;
                creation.creationTail = decoded.creationTail;
                creation.creatorTrailer = decoded.creatorTrailer;
                result = state::prepare_character_creation(creation, mutation);
            }

            const bool queuezReady =
                result == state::CharacterCreationResult::ok && queuez::valid(queuezState)
                && queuezState.family4Active && queuezState.family3Active
                && queuezState.family4RootSoid == mutation.accountSoid
                && queuezState.family3RootSoid == mutation.accountSoid
                && queuezState.family3Phase == queuez::Family3Phase::normal
                && (!mutation.selectCreated || queuezState.family0Active);
            std::array<char, core::log::kLineCapacity> line{};
            const int count = std::snprintf(
                line.data(),
                line.size(),
                "ev=character_create stage=prepare result=%s reason=%s tx=%u payload=%zu "
                "race=%u gender=%u class=%u trailer=%u soid=0x%llX select=%u",
                queuezReady ? "ok" : "fail",
                state::character_creation_result_name(result),
                message.transactionId,
                message.payload.size(),
                static_cast<unsigned>(decoded.race),
                static_cast<unsigned>(decoded.gender),
                static_cast<unsigned>(decoded.characterClass),
                static_cast<unsigned>(decoded.creatorTrailer),
                static_cast<unsigned long long>(mutation.characterSoid),
                mutation.selectCreated ? 1U : 0U);
            if (count > 0) {
                core::log::write(core::log::Channel::server,
                                 queuezReady ? core::log::Level::info : core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(count)});
            }
            if (!queuezReady) {
                return encode_create_fallback(message, output, written);
            }
            if (!middleware::web_service::messages::opcode501::encode_response(
                    message, mutation.characterSoid, output, written)) {
                return false;
            }
            auto& transaction = outcome.transaction.emplace<CharacterCreationTransaction>();
            transaction.pending = mutation;
            return true;
        }
        if (parsedMessage
            && message.opcode == middleware::web_service::messages::opcode502::kOpcode) {
            middleware::web_service::messages::opcode502::Request request{};
            state::PendingCharacterDeletion mutation{};
            state::CharacterDeletionResult result = state::CharacterDeletionResult::invalid;
            if (middleware::web_service::messages::opcode502::parse_request(message, request)) {
                result = state::prepare_character_deletion(request.characterSoid, mutation);
            }

            const bool prepared = result == state::CharacterDeletionResult::ok;
            std::array<char, core::log::kLineCapacity> line{};
            const int count = std::snprintf(
                line.data(),
                line.size(),
                "ev=character_delete stage=prepare result=%s reason=%s tx=%u payload=%zu "
                "soid=0x%llX before=%zu index=%zu",
                prepared ? "ok" : "fail",
                state::character_deletion_result_name(result),
                message.transactionId,
                message.payload.size(),
                static_cast<unsigned long long>(request.characterSoid),
                mutation.beforeCharacterCount,
                mutation.characterIndex);
            if (count > 0) {
                core::log::write(core::log::Channel::server,
                                 prepared ? core::log::Level::info : core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(count)});
            }

            middleware::web_service::StatusResponse status{};
            status.code = prepared ? 0 : 1;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                return false;
            }
            if (prepared) {
                auto& transaction = outcome.transaction.emplace<CharacterDeletionTransaction>();
                transaction.pending = mutation;
            }
            return true;
        }
        if (parsedMessage
            && message.opcode == middleware::web_service::messages::opcode505::kOpcode) {
            if (!middleware::web_service::messages::opcode505::parse_request(message)
                || !queuez::stage_change_character(queuezState, outcome.changeCharacter)
                || !middleware::web_service::messages::opcode505::encode_response(
                    message, outcome.changeCharacter.after.family4Version, output, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws505 stage=change result=fail");
                outcome.changeCharacter = {};
                return middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    middleware::web_service::StatusResponse{},
                    output,
                    written);
            }
            outcome.hasChangeCharacter = true;
            return true;
        }
        web_service::Outcome webOutcome;
        if (!sunrise::server::web_service::consume(requestBody, output, written, webOutcome)) {
            return false;
        }
        outcome.hasSubscription = webOutcome.hasSubscription;
        outcome.subscription = webOutcome.subscription;
        const auto* equipmentSwap =
            web_service::mutation_if<state::PendingEquipmentSwap>(webOutcome);
        const auto* subclassSelection =
            web_service::mutation_if<state::PendingSubclassSelection>(webOutcome);
        const auto* socketPlug = web_service::mutation_if<state::PendingSocketPlug>(webOutcome);
        const auto* itemState = web_service::mutation_if<state::PendingItemState>(webOutcome);
        const auto* itemAcquisition =
            web_service::mutation_if<state::PendingItemAcquisition>(webOutcome);
        const auto* profileItemAcquisition =
            web_service::mutation_if<state::PendingProfileItemAcquisition>(webOutcome);
        const auto* itemDismantle =
            web_service::mutation_if<state::PendingItemDismantle>(webOutcome);
        if (equipmentSwap != nullptr) {
            auto& transaction = outcome.transaction.emplace<EquipmentSwapTransaction>();
            if (!queuez::stage_equipment_swap(
                    queuezState, equipmentSwap->characterSoid, transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws403 stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=ws403 stage=response result=fail");
                    return false;
                }
                web_service::report_equip_response(message, status.value, output.first(written));
                transaction.pending = *equipmentSwap;
            }
        }
        if (subclassSelection != nullptr) {
            auto& transaction = outcome.transaction.emplace<SubclassSelectionTransaction>();
            if (!queuez::stage_subclass_selection(queuezState,
                                                  subclassSelection->accountSoid,
                                                  subclassSelection->characterSoid,
                                                  subclassSelection->subclassInstanceSoid,
                                                  transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=subclass_select stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=subclass_select stage=response result=fail");
                    return false;
                }
                web_service::report_subclass_selection_response(
                    message, status.value, *subclassSelection, output.first(written));
                transaction.pending = *subclassSelection;
            }
        }
        if (socketPlug != nullptr) {
            auto& transaction = outcome.transaction.emplace<SocketPlugTransaction>();
            if (!queuez::stage_socket_plug(queuezState,
                                           socketPlug->accountSoid,
                                           socketPlug->characterSoid,
                                           socketPlug->targetInstanceSoid,
                                           socketPlug->profileChanged,
                                           transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=socket_plug stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=socket_plug stage=response result=fail");
                    return false;
                }
                web_service::report_socket_plug_response(message,
                                                         status.value,
                                                         socketPlug->targetInstanceSoid,
                                                         socketPlug->socketLane,
                                                         socketPlug->plugDefinitionIndex,
                                                         output.first(written));
                transaction.pending = *socketPlug;
            }
        }
        if (itemState != nullptr) {
            auto& transaction = outcome.transaction.emplace<ItemStateTransaction>();
            if (!queuez::stage_equipment_swap(
                    queuezState, itemState->characterSoid, transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=item_state stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=item_state stage=response result=fail");
                    return false;
                }
                transaction.pending = *itemState;
            }
        }
        if (itemAcquisition != nullptr) {
            auto& transaction = outcome.transaction.emplace<ItemAcquisitionTransaction>();
            if (!queuez::stage_item_acquisition(queuezState,
                                                itemAcquisition->accountSoid,
                                                itemAcquisition->characterSoid,
                                                itemAcquisition->acquiredInstanceSoid,
                                                itemAcquisition->profileChanged,
                                                transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=acquire stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=acquire stage=response result=fail");
                    return false;
                }
                web_service::report_item_acquisition_response(message,
                                                              status.value,
                                                              itemAcquisition->acquiredInstanceSoid,
                                                              output.first(written));
                transaction.pending = *itemAcquisition;
            }
        }
        if (profileItemAcquisition != nullptr) {
            auto& transaction = outcome.transaction.emplace<ProfileItemAcquisitionTransaction>();
            if (!queuez::stage_profile_item_acquisition(
                    queuezState,
                    profileItemAcquisition->accountSoid,
                    profileItemAcquisition->acquiredInstanceSoid,
                    profileItemAcquisition->actionSource,
                    profileItemAcquisition->appended,
                    transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=profile_acquire stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=profile_acquire stage=response result=fail");
                    return false;
                }
                web_service::report_profile_item_acquisition_response(
                    message,
                    status.value,
                    profileItemAcquisition->acquiredDefinitionHash,
                    profileItemAcquisition->acquiredQuantity,
                    output.first(written));
                transaction.pending = *profileItemAcquisition;
            }
        }
        if (itemDismantle != nullptr) {
            auto& transaction = outcome.transaction.emplace<ItemDismantleTransaction>();
            if (!queuez::stage_item_dismantle(queuezState,
                                              itemDismantle->accountSoid,
                                              itemDismantle->characterSoid,
                                              itemDismantle->dismantledInstanceSoid,
                                              itemDismantle->profileChanged,
                                              transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=dismantle stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=dismantle stage=response result=fail");
                    return false;
                }
                web_service::report_item_dismantle_response(message,
                                                            status.value,
                                                            itemDismantle->dismantledInstanceSoid,
                                                            output.first(written));
                transaction.pending = *itemDismantle;
            }
        }
        if (webOutcome.hasSelectedCharacter
            && queuez::stage_select_character(
                queuezState, webOutcome.selectedCharacterSoid, outcome.selectCharacter)) {
            outcome.hasSelectCharacter = true;
        } else {
            outcome.selectCharacter = {};
        }
        return true;
    }
    }
    written = 0;
    return false;
}

} // namespace sunrise::server::bap::encrypted::body
