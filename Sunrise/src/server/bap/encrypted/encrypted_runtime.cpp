#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../middleware/secure_channel/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../internal.h"
#include "activity_transaction/activity_transaction_notifications.h"
#include "bap_connection_publication.h"
#include "internal.h"
#include "push/activity/activity_roster_push.h"
#include "queuez/queuez_outcome_staging.h"
#include "transactions/service_outcome_commit.h"

namespace sunrise::server::bap::encrypted {
namespace {

void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}

} // namespace

bool consume(Session& session,
             Scratch& scratch,
             const middleware::bap::OuterFrame& outer,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    written = 0;
    session.accountMutationPublished = false;
    if (!session.authenticated) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap stage=encrypted result=drop reason=unauthenticated");
        return false;
    }

    std::size_t plaintextSize = 0;
    const auto& bapState = state::bap();
    if (!middleware::secure_channel::open_frame(bapState.sessionKey,
                                                session.receiveNonce,
                                                outer.payload,
                                                scratch.plaintext,
                                                plaintextSize)) {
        const std::size_t possiblePlaintextSize =
            outer.payload.size() >= middleware::secure_channel::kFrameTagSize
                ? outer.payload.size() - middleware::secure_channel::kFrameTagSize
                : 0;
        clear_prefix(scratch.plaintext, possiblePlaintextSize);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=none stage=decrypt result=fail");
        return false;
    }
    middleware::secure_channel::advance_nonce(session.receiveNonce);

    middleware::bap::RequestFrame frame;
    ServiceRoute route;
    std::size_t responseBodySize = 0;
    std::size_t framedSize = 0;
    ServiceOutcome outcome{};
    transactions::Publication publication{};
    queuez::SessionState nextQueuez = session.queuez;
    bool publishesQueuez = false;
    bool handled =
        middleware::bap::parse_request_payload(std::span(scratch.plaintext).first(plaintextSize),
                                               middleware::bap::FrameType::encrypted,
                                               frame)
        && routing::resolve(frame.messageId, route);
    if (!handled) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=none stage=parse result=fail");
    }
    const bool processesBody = handled && route.responseMode != ResponseMode::none;
    const bool sendsReply = handled && route.responseMode == ResponseMode::reply;
    if (processesBody
        && !body::process(route,
                          session.queuez,
                          session.activity,
                          session.matchmakingContext,
                          frame.body,
                          scratch.responseBody,
                          responseBodySize,
                          outcome)) {
        diagnostics::report_failure(frame.messageId, "body");
        clear_prefix(scratch.responseBody, responseBodySize);
        responseBodySize = 0;
        outcome = {};
        handled = sendsReply;
    }
    if (handled && sendsReply) {
        handled = reply::encode(scratch,
                                route,
                                frame.taskId,
                                bapState.sessionKey,
                                session.sendNonce,
                                std::span(scratch.responseBody).first(responseBodySize),
                                framedSize);
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "encode");
        }
    }

    auto nextSendNonce = session.sendNonce;
    if (handled && sendsReply) {
        middleware::secure_channel::advance_nonce(nextSendNonce);
    }
    queuez::StagedPublication queuezPublication{};
    if (handled) {
        handled = queuez::stage_service_outcome(scratch,
                                                session.queuez,
                                                outcome,
                                                bapState.sessionKey,
                                                nextSendNonce,
                                                scratch.framed,
                                                framedSize,
                                                queuezPublication);
        if (handled && queuezPublication.hasState) {
            nextQueuez = queuezPublication.after;
            publishesQueuez = true;
        }
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "stage");
        }
    }
    const auto* activityPlan = transaction_if<activity_message::ActivityPlan>(outcome);
    if (handled && activityPlan != nullptr) {
        handled = route.responseMode == ResponseMode::uncorrelatedPush;
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "route");
        } else if (!activity_transaction::stage_notifications(session,
                                                              scratch,
                                                              *activityPlan,
                                                              bapState.sessionKey,
                                                              nextSendNonce,
                                                              scratch.framed,
                                                              framedSize)) {
            diagnostics::report_failure(frame.messageId, "notify");
        }
    }
    const bool mutatesAccount =
        outcome.hasChangeCharacter || outcome.hasSelectCharacter
        || transaction_if<CharacterCreationTransaction>(outcome) != nullptr
        || transaction_if<CharacterDeletionTransaction>(outcome) != nullptr
        || transaction_if<EquipmentSwapTransaction>(outcome) != nullptr
        || transaction_if<SocketPlugTransaction>(outcome) != nullptr
        || transaction_if<ItemStateTransaction>(outcome) != nullptr
        || transaction_if<ItemAcquisitionTransaction>(outcome) != nullptr
        || transaction_if<ProfileItemAcquisitionTransaction>(outcome) != nullptr
        || transaction_if<ItemDismantleTransaction>(outcome) != nullptr;

    const auto* stagedCreation = transaction_if<CharacterCreationTransaction>(outcome);
    const std::uint64_t createdCharacterSoid =
        stagedCreation == nullptr ? 0 : stagedCreation->pending.characterSoid;
    const auto* stagedDeletion = transaction_if<CharacterDeletionTransaction>(outcome);
    const std::uint64_t deletedCharacterSoid =
        stagedDeletion == nullptr ? 0 : stagedDeletion->pending.characterSoid;
    const auto* stagedSocket = transaction_if<SocketPlugTransaction>(outcome);
    const std::uint8_t socketLane = stagedSocket == nullptr ? 0 : stagedSocket->pending.socketLane;
    const std::uint16_t socketPlugDefinition =
        stagedSocket == nullptr ? 0 : stagedSocket->pending.plugDefinitionIndex;
    const std::uint8_t socketTargetBucket =
        stagedSocket == nullptr ? 0 : stagedSocket->pending.targetBucketId;
    const std::uint8_t socketPlugBucket =
        stagedSocket == nullptr ? 0 : stagedSocket->pending.plugBucketId;
    const auto* stagedItemState = transaction_if<ItemStateTransaction>(outcome);
    const std::uint64_t itemStateInstance =
        stagedItemState == nullptr ? 0 : stagedItemState->pending.targetInstanceSoid;
    const std::uint32_t itemStateFlags =
        stagedItemState == nullptr ? 0 : stagedItemState->pending.afterFlags;
    const auto* stagedProfile = transaction_if<ProfileItemAcquisitionTransaction>(outcome);
    const std::uint32_t profileDefinitionHash =
        stagedProfile == nullptr ? 0 : stagedProfile->pending.acquiredDefinitionHash;
    const std::int32_t profileQuantity =
        stagedProfile == nullptr ? 0 : stagedProfile->pending.acquiredQuantity;
    const bool profileActionSource =
        stagedProfile != nullptr && stagedProfile->pending.actionSource;
    const bool profileAppended = stagedProfile != nullptr && stagedProfile->pending.appended;
    const ConnectionFields connection = connection_fields(outcome);
    if (handled && processesBody) {
        handled = framedSize <= response.size() && transactions::commit(outcome, publication);
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "commit");
        }
        if (handled) {
            std::copy_n(scratch.framed.begin(), framedSize, response.begin());
            written = framedSize;
            session.sendNonce = nextSendNonce;
            if (publishesQueuez) {
                session.queuez = nextQueuez;
            }
            arm_repushes(session, queuezPublication);
            publish_connection_fields(session, publication, connection);
            push::activity::commit_staged_roster(session);
            commit_staged_advertisement(session);
            if (activityPlan != nullptr && framedSize != 0) {
                session.activityKeepaliveDueTick = GetTickCount64() + kActivityKeepaliveIntervalMs;
            }
            session.accountMutationPublished = mutatesAccount;
            if (createdCharacterSoid != 0) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=character_create stage=output_publish result=ok framed_bytes=%zu "
                    "queuez_published=%u family_version=%d family0_version=%d family3_version=%d "
                    "character=0x%llX",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    session.queuez.family0Version,
                    session.queuez.family3Version,
                    static_cast<unsigned long long>(createdCharacterSoid));
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::info,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
            if (deletedCharacterSoid != 0) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=character_delete stage=output_publish result=ok framed_bytes=%zu "
                    "queuez_published=%u family_version=%d family0_version=%d family3_version=%d "
                    "character=0x%llX",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    session.queuez.family0Version,
                    session.queuez.family3Version,
                    static_cast<unsigned long long>(deletedCharacterSoid));
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::info,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
            if (transaction_if<EquipmentSwapTransaction>(outcome) != nullptr) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=equip stage=output_publish result=ok framed_bytes=%zu queuez_published=%u "
                    "family_version=%d family0_version=%d family3_version=%d",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    session.queuez.family0Version,
                    session.queuez.family3Version);
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::debug,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
            if (const auto* transaction = transaction_if<SocketPlugTransaction>(outcome)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=socket_plug stage=output_publish result=ok framed_bytes=%zu "
                    "queuez_published=%u family_version=%d family0_version=%d "
                    "family3_version=%d instance=0x%llX lane=%u "
                    "plug_definition=%u target_bucket=%u plug_bucket=%u",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    session.queuez.family0Version,
                    session.queuez.family3Version,
                    static_cast<unsigned long long>(transaction->update.targetInstanceSoid),
                    static_cast<unsigned>(socketLane),
                    static_cast<unsigned>(socketPlugDefinition),
                    static_cast<unsigned>(socketTargetBucket),
                    static_cast<unsigned>(socketPlugBucket));
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::debug,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
            if (const auto* transaction = transaction_if<ItemStateTransaction>(outcome)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=item_state stage=output_publish result=ok framed_bytes=%zu "
                    "queuez_published=%u family_version=%d instance=0x%llX flags=0x%X",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    static_cast<unsigned long long>(itemStateInstance),
                    itemStateFlags);
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::debug,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
            if (const auto* transaction = transaction_if<ItemAcquisitionTransaction>(outcome)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=acquire stage=output_publish result=ok framed_bytes=%zu "
                    "queuez_published=%u family_version=%d residents=%u instance=0x%llX",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    static_cast<unsigned>(session.queuez.family4ResidentCount),
                    static_cast<unsigned long long>(transaction->update.acquiredInstanceSoid));
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::debug,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
            if (const auto* transaction =
                    transaction_if<ProfileItemAcquisitionTransaction>(outcome)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=profile_acquire stage=output_publish result=ok framed_bytes=%zu "
                    "queuez_published=%u family_version=%d residents=%u definition_hash=0x%08X "
                    "quantity=%d instance=0x%llX action_source=%u appended_row=%u "
                    "appended_resident=%u",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    static_cast<unsigned>(session.queuez.family4ResidentCount),
                    profileDefinitionHash,
                    profileQuantity,
                    static_cast<unsigned long long>(transaction->update.acquiredInstanceSoid),
                    static_cast<unsigned>(profileActionSource),
                    static_cast<unsigned>(profileAppended),
                    static_cast<unsigned>(transaction->update.appendedResident));
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::debug,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
            if (const auto* transaction = transaction_if<ItemDismantleTransaction>(outcome)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int count = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=dismantle stage=output_publish result=ok framed_bytes=%zu "
                    "queuez_published=%u family_version=%d residents=%u instance=0x%llX",
                    framedSize,
                    static_cast<unsigned>(publishesQueuez),
                    session.queuez.family4Version,
                    static_cast<unsigned>(session.queuez.family4ResidentCount),
                    static_cast<unsigned long long>(transaction->update.dismantledInstanceSoid));
                if (count > 0) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::debug,
                                     {line.data(), static_cast<std::size_t>(count)});
                }
            }
        }
    }
    if (!handled) {
        push::activity::discard_staged_roster(session);
        discard_staged_advertisement(session);
    }
    clear_prefix(scratch.plaintext, plaintextSize);
    clear_prefix(scratch.responseBody, responseBodySize);
    clear_prefix(scratch.framed, framedSize);
    outcome = {};
    SecureZeroMemory(&publication, sizeof publication);
    SecureZeroMemory(&queuezPublication, sizeof queuezPublication);
    if (handled) {
        core::log::write(core::log::Channel::server, core::log::Level::info, route.successEvent);
    }
    return handled;
}

} // namespace sunrise::server::bap::encrypted
