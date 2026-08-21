#include "service_outcome_commit.h"

#include <array>
#include <cstdio>

#include "../../../../client/content/investment/worker.h"
#include "../../../../core/logging/log.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/matchmaking/matchmaking_state.h"
#include "../../../../state/runtime/character_creation.h"
#include "../../../../state/runtime/character_deletion.h"
#include "../../../../state/runtime/runtime.h"
#include "../bap_connection_publication.h"
#include "../internal.h"

namespace sunrise::server::bap::encrypted::transactions {
namespace {

namespace slots = state::activity::entity_slots;
constexpr std::array<const char*, 4> kLeaseKinds = {"none", "join", "grant", "release"};

[[nodiscard]] bool retain_private(std::uint64_t sessionId, Publication& publication) noexcept {
    state::activity::SessionBinding binding{};
    if (!state::activity::snapshot_binding(sessionId, binding)
        || !state::activity::retain_binding(binding)) {
        return false;
    }
    publication.activity.session = binding;
    publication.activity.source = binding;
    publication.activity.role = ActivityClientRole::privateCurrent;
    publication.hasActivitySessionBinding = true;
    return true;
}

[[nodiscard]] bool retain_public(const activity_message::ActivityPlan& plan,
                                 Publication& publication) noexcept {
    server::gameplay::group::HostSessionBinding current{};
    if (!server::gameplay::group::host_session_for_activity(plan.sessionId, current)
        || current.generation != plan.publicHost.generation
        || current.groupSessionId != plan.publicHost.groupSessionId
        || current.regionIndex != plan.publicHost.regionIndex
        || current.source.sessionId != plan.publicHost.source.sessionId
        || current.source.createdRevision != plan.publicHost.source.createdRevision
        || current.target.sessionId != plan.publicHost.target.sessionId
        || current.target.createdRevision != plan.publicHost.target.createdRevision
        || !state::activity::binding_matches(current.source)
        || !state::activity::binding_matches(current.target)
        || !server::gameplay::group::retain_host_session(current.generation)) {
        return false;
    }
    if (!state::activity::retain_binding(current.target)) {
        server::gameplay::group::release_host_session(current.generation);
        return false;
    }
    publication.activity.session = current.target;
    publication.activity.source = current.source;
    publication.activity.groupSessionId = current.groupSessionId;
    publication.activity.hostGeneration = current.generation;
    publication.activity.advertisedRegion = current.regionIndex;
    publication.activity.role = ActivityClientRole::publicTarget;
    publication.hasActivitySessionBinding = true;
    return true;
}

void discard_activity_publication(Publication& publication) noexcept {
    if (publication.activity.hostGeneration != 0) {
        server::gameplay::group::release_host_session(publication.activity.hostGeneration);
    }
    if (publication.activity.session.sessionId != state::activity::kAbsentSessionId) {
        state::activity::release_binding(publication.activity.session);
    }
    publication = {};
}

void report_lease(const slots::PendingMutation& mutation, bool committed) noexcept {
    std::size_t held = 0;
    std::size_t reserved = 0;
    const bool known = slots::lease_counts(mutation.sessionId, held, reserved);
    const auto kind = static_cast<std::size_t>(mutation.kind);
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=entity_slots result=%s kind=%s soid=0x%llX "
                      "requested=%zu picked=%zu held=%zu reserved=%zu known=%u",
                      committed ? "ok" : "fail",
                      kind < kLeaseKinds.size() ? kLeaseKinds[kind] : "bad",
                      static_cast<unsigned long long>(mutation.sessionId),
                      mutation.requestedCount,
                      slots::slot_count(mutation.mask),
                      held,
                      reserved,
                      known ? 1U : 0U);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

bool commit(ServiceOutcome& outcome, Publication& publication) noexcept {
    publication = {};
    if (auto* allocation = transaction_if<state::activity::PendingAllocation>(outcome)) {
        const std::uint64_t sessionId = allocation->sessionId;
        std::uint64_t bindingGeneration = 0;
        if (sessionId == state::activity::kAbsentSessionId
            || !reserve_activity_binding_generation(bindingGeneration)
            || !state::activity::commit(*allocation)) {
            return false;
        }
        if (!retain_private(sessionId, publication)) {
            static_cast<void>(state::activity::release_session(sessionId));
            return false;
        }
        publication.activity.bindingGeneration = bindingGeneration;
        return true;
    }
    if (auto* plan = transaction_if<activity_message::ActivityPlan>(outcome)) {
        if (plan->mutationDomain == activity_message::MutationDomain::entitySlots) {
            const bool joins = plan->delivery == activity_message::Delivery::joinNotifications;
            const bool validJoinIntent =
                plan->bindingIntent == activity_message::BindingIntent::preserveCurrent
                || plan->bindingIntent == activity_message::BindingIntent::publicTarget;
            if (joins && !validJoinIntent) {
                return false;
            }
            std::uint64_t bindingGeneration = 0;
            if (joins && !reserve_activity_binding_generation(bindingGeneration)) {
                return false;
            }
            if (joins && plan->bindingIntent == activity_message::BindingIntent::publicTarget
                && !retain_public(*plan, publication)) {
                return false;
            }
            const slots::PendingMutation attempted = plan->entitySlotMutation;
            const bool committed = slots::commit(plan->entitySlotMutation);
            report_lease(attempted, committed);
            if (!committed) {
                discard_activity_publication(publication);
                return false;
            }
            if (joins && plan->sessionId != state::activity::kAbsentSessionId) {
                publication.hasActivitySessionBinding = true;
                if (plan->bindingIntent == activity_message::BindingIntent::preserveCurrent) {
                    publication.preservesActivitySessionBinding = true;
                } else if (plan->bindingIntent != activity_message::BindingIntent::publicTarget) {
                    discard_activity_publication(publication);
                    return false;
                }
                publication.activity.bindingGeneration = bindingGeneration;
            }
            return true;
        }
        if (plan->mutationDomain == activity_message::MutationDomain::membership) {
            return state::activity::membership::commit(plan->membershipMutation);
        }
        return plan->mutationDomain == activity_message::MutationDomain::patchEpoch;
    }
    if (auto* mutation = transaction_if<state::matchmaking::PendingMutation>(outcome)) {
        return state::matchmaking::commit(*mutation);
    }
    if (auto* transaction = transaction_if<CharacterCreationTransaction>(outcome)) {
        const bool committed = state::commit_character_creation(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=character_create stage=transaction_commit result=ok"
                                   : "ev=character_create stage=transaction_commit result=fail");
        return committed;
    }
    if (auto* transaction = transaction_if<CharacterDeletionTransaction>(outcome)) {
        const bool committed = state::commit_character_deletion(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=character_delete stage=transaction_commit result=ok"
                                   : "ev=character_delete stage=transaction_commit result=fail");
        return committed;
    }
    if (auto* transaction = transaction_if<EquipmentSwapTransaction>(outcome)) {
        const bool isSubclassSlot =
            transaction->pending.equipmentSlotIndex
            == static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);
        const bool committed = state::commit_equipment_swap(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=equip stage=transaction_commit result=ok"
                                   : "ev=equip stage=transaction_commit result=fail");
        if (committed && isSubclassSlot) {
            client::content::investment::worker::request_slice();
        }
        return committed;
    }
    if (auto* transaction = transaction_if<SubclassSelectionTransaction>(outcome)) {
        const bool committed = state::commit_subclass_selection(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=subclass_select stage=transaction_commit result=ok"
                                   : "ev=subclass_select stage=transaction_commit result=fail");
        if (committed) {
            client::content::investment::worker::request_slice();
        }
        return committed;
    }
    if (auto* transaction = transaction_if<ItemAcquisitionTransaction>(outcome)) {
        const bool committed = state::commit_item_acquisition(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=acquire stage=transaction_commit result=ok"
                                   : "ev=acquire stage=transaction_commit result=fail");
        return committed;
    }
    if (auto* transaction = transaction_if<SocketPlugTransaction>(outcome)) {
        const bool committed = state::commit_socket_plug(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=socket_plug stage=transaction_commit result=ok"
                                   : "ev=socket_plug stage=transaction_commit result=fail");
        return committed;
    }
    if (auto* transaction = transaction_if<ItemStateTransaction>(outcome)) {
        const bool committed = state::commit_item_state(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=item_state stage=transaction_commit result=ok"
                                   : "ev=item_state stage=transaction_commit result=fail");
        return committed;
    }
    if (auto* transaction = transaction_if<ProfileItemAcquisitionTransaction>(outcome)) {
        const bool committed = state::commit_profile_item_acquisition(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=profile_acquire stage=transaction_commit result=ok"
                                   : "ev=profile_acquire stage=transaction_commit result=fail");
        return committed;
    }
    if (auto* transaction = transaction_if<ItemDismantleTransaction>(outcome)) {
        const bool committed = state::commit_item_dismantle(transaction->pending);
        core::log::write(core::log::Channel::server,
                         committed ? core::log::Level::debug : core::log::Level::warn,
                         committed ? "ev=dismantle stage=transaction_commit result=ok"
                                   : "ev=dismantle stage=transaction_commit result=fail");
        return committed;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::transactions
