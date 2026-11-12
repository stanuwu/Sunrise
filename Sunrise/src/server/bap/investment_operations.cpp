#include "investment_operations.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "../../core/logging/log.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::server::bap {
namespace {

/** The request type an operation kind names. */
template <InvestmentOperationKind kind>
using KindRequest = std::variant_alternative_t<static_cast<std::size_t>(kind), InvestmentRequest>;
static_assert(std::variant_size_v<InvestmentRequest>
              == static_cast<std::size_t>(InvestmentOperationKind::completePursuitObjectives) + 1);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::grantItem>, GrantItemOperation>);
static_assert(
    std::is_same_v<KindRequest<InvestmentOperationKind::setQuantity>, state::QuantityEdit>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::setSeasonalExperience>,
                             SeasonalExperienceOperation>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::claimSeasonPassReward>,
                             SeasonPassClaimOperation>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::resetSeasonPassClaim>,
                             SeasonPassResetOperation>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::setPursuitObjective>,
                             ObjectiveProgressOperation>);
static_assert(std::is_same_v<KindRequest<InvestmentOperationKind::completePursuitObjectives>,
                             ObjectiveCompletionOperation>);

/** One submitted operation, the editor process that submitted it, and where it stands. */
struct RetainedOperation {
    InvestmentOperation operation{};
    std::uint32_t editorPid{};
    InvestmentReceipt receipt{};
    /** Quantity the grant's earlier steps have already published. */
    std::uint32_t grantedQuantity{};
};

/**
 * Every receipt an editor has not yet acknowledged, oldest first. Guarded by the session lock.
 * Each entry is one explicit editor action, which the editor acknowledges once it reads the
 * finished receipt. A dropped link refuses whatever is still pending and a connecting link forgets
 * the receipts of exited editors, so the table needs no capacity of its own. Only a failed
 * allocation refuses an entry.
 */
std::vector<RetainedOperation> g_operations{};

/** @return True for a final state, which never changes again. */
[[nodiscard]] bool finished(InvestmentReceiptState state) noexcept {
    return state == InvestmentReceiptState::refused || state == InvestmentReceiptState::published
           || state == InvestmentReceiptState::committedUnpublished;
}

/** @return The retained operation, or null. Caller owns the session lock. */
[[nodiscard]] RetainedOperation* find_operation(std::uint64_t operationId) noexcept {
    const auto found =
        std::find_if(g_operations.begin(), g_operations.end(), [&](const RetainedOperation& entry) {
            return entry.operation.id == operationId;
        });
    return found == g_operations.end() ? nullptr : &*found;
}

/** @return The authenticated Family-4 peer rooted at the account, or null. Caller owns the lock. */
[[nodiscard]] const Session* publication_peer(std::uint64_t accountSoid) noexcept {
    for (const Session& session : sessions()) {
        if (session.id != 0 && session.authenticated && session.queuez.family4Active
            && session.queuez.family4RootSoid == accountSoid) {
            return &session;
        }
    }
    return nullptr;
}

/**
 * Copies a receipt for the editor. A pending operation reads as deferred while no peer of its
 * account can run it, or while that peer must resync first. Caller owns the session lock.
 */
[[nodiscard]] InvestmentReceipt present(const RetainedOperation& retained) noexcept {
    InvestmentReceipt receipt = retained.receipt;
    if (finished(receipt.state)) {
        return receipt;
    }
    const Session* peer = publication_peer(receipt.target.accountSoid);
    if (peer == nullptr) {
        receipt.state = InvestmentReceiptState::deferred;
        receipt.wait = InvestmentWait::publicationPeer;
    } else if (peer->accountResyncArmed) {
        receipt.state = InvestmentReceiptState::deferred;
        receipt.wait = InvestmentWait::accountResync;
    }
    return receipt;
}

/** Applies a transition to a receipt. */
void apply_transition(InvestmentReceipt& receipt, const InvestmentTransition& transition) noexcept {
    receipt.state = transition.state;
    receipt.wait = transition.wait;
    receipt.reason = transition.reason != nullptr ? transition.reason : "";
}

/**
 * Merges a later step into a grant that has already published part of its quantity. A refused
 * step finishes the grant as published with the refusal reason, because the earlier steps stay
 * granted.
 */
[[nodiscard]] InvestmentTransition continue_grant(InvestmentTransition step) noexcept {
    if (step.state == InvestmentReceiptState::refused) {
        step.state = InvestmentReceiptState::published;
    }
    return step;
}

/** Logs a receipt as it stands after one step, such as submit or execute. */
void report_receipt(const char* stage, const InvestmentReceipt& receipt) noexcept {
    core::log::writef(core::log::Channel::server,
                      receipt.state == InvestmentReceiptState::committedUnpublished
                          ? core::log::Level::warn
                          : core::log::Level::info,
                      "ev=studio_operation stage=%s id=%llu kind=%u state=%u wait=%u reason=%s",
                      stage,
                      static_cast<unsigned long long>(receipt.operationId),
                      static_cast<unsigned>(receipt.kind),
                      static_cast<unsigned>(receipt.state),
                      static_cast<unsigned>(receipt.wait),
                      receipt.reason);
}

/** @return True while any item table that inventory grants and edits read is still loading. */
[[nodiscard]] bool inventory_catalogs_loading() noexcept {
    return !state::build_data::item_definitions_ready()
           || !state::build_data::configured_item_details_ready()
           || !state::build_data::inventory_bucket_descriptors_ready();
}

/** @return Queued when the live account, character and content still match the target. */
[[nodiscard]] InvestmentTransition check_target(const InvestmentTarget& target) noexcept {
    state::AccountIdentity account{};
    if (!state::account_identity(account)) {
        return refused_transition("account_state");
    }
    if (account.primarySoid != target.accountSoid) {
        return refused_transition("account_changed");
    }
    const std::uint64_t selected = state::account::selected_character_soid(account);
    if (selected == 0 || selected != target.characterSoid) {
        return refused_transition("character_changed");
    }
    if (target.contentHash != state::build_data::content_hash()) {
        return refused_transition("catalog_changed");
    }
    return {};
}

/**
 * A grant must name an installed item that State can route, and a positive quantity its
 * destination can hold at once.
 */
[[nodiscard]] InvestmentTransition check_request(const GrantItemOperation& request) noexcept {
    if (inventory_catalogs_loading()) {
        return deferred_transition(InvestmentWait::catalog);
    }
    // Routing and granting both read the reward graph, as a claim does.
    if (state::build_data::reward_definitions_unsupported()) {
        return refused_transition("rewards_unsupported");
    }
    if (!state::build_data::reward_definitions_ready()) {
        return deferred_transition(InvestmentWait::catalog);
    }
    state::build_data::items::Definition item{};
    if (!state::build_data::find_item_definition_hash(request.definitionHash, item)) {
        return refused_transition("item_definition");
    }
    // The grant path can place any routable item, one instance or stack per step.
    if (!state::item_grantable(item.definitionIndex)) {
        return refused_transition("item_route");
    }
    // Each step takes a poll ahead of the peer's other pushes, and past this capacity a step only
    // evicts or drops an earlier one. The remainder is checked again before every step.
    const std::int32_t capacity = state::item_grant_capacity(item.definitionIndex);
    return request.quantity == 0 || request.quantity > static_cast<std::uint32_t>(capacity)
               ? refused_transition("quantity")
               : InvestmentTransition{};
}

/** A quantity edit must name an installed item and a quantity State can store. */
[[nodiscard]] InvestmentTransition check_request(const state::QuantityEdit& edit) noexcept {
    if (inventory_catalogs_loading()) {
        return deferred_transition(InvestmentWait::catalog);
    }
    const char* reason = nullptr;
    return state::check_quantity_edit(edit, &reason) ? InvestmentTransition{}
                                                     : refused_transition(reason);
}

/** Only the range is checked here. State compares the expected total when the edit runs. */
[[nodiscard]] InvestmentTransition
check_request(const SeasonalExperienceOperation& request) noexcept {
    const char* reason = nullptr;
    return state::check_seasonal_experience(request.replacement, &reason)
               ? InvestmentTransition{}
               : refused_transition(reason);
}

/**
 * A pass operation must name an installed row that still grants the expected item and that the
 * earned-perk walk does not grant. Finding the walk's rows needs the reward graph.
 */
[[nodiscard]] InvestmentTransition check_season_pass_row(std::uint16_t rewardIndex,
                                                         std::uint32_t itemHash) noexcept {
    if (state::build_data::reward_definitions_unsupported()) {
        return refused_transition("rewards_unsupported");
    }
    const char* reason = nullptr;
    return state::check_season_pass_reward(rewardIndex, itemHash, &reason)
               ? InvestmentTransition{}
               : state_refusal_transition(reason);
}

/** The opcode-2400 claim rules run when the claim does. */
[[nodiscard]] InvestmentTransition check_request(const SeasonPassClaimOperation& request) noexcept {
    return check_season_pass_row(request.rewardIndex, request.itemHash);
}

/** A reset only clears the row's claim flag. */
[[nodiscard]] InvestmentTransition check_request(const SeasonPassResetOperation& request) noexcept {
    return check_season_pass_row(request.rewardIndex, request.itemHash);
}

/** A pursuit edit must name an installed item. State checks the held instance when it runs. */
[[nodiscard]] InvestmentTransition check_pursuit_item(std::uint32_t itemHash) noexcept {
    if (state::build_data::pursuit_catalog_unsupported()) {
        return refused_transition("pursuits_unsupported");
    }
    if (!state::build_data::item_definitions_ready()
        || !state::build_data::objective_definitions_ready()
        || !state::build_data::pursuit_definitions_ready()) {
        return deferred_transition(InvestmentWait::catalog);
    }
    state::build_data::items::Definition item{};
    return state::build_data::find_item_definition_hash(itemHash, item)
               ? InvestmentTransition{}
               : refused_transition("item_definition");
}

/** State checks the objective, its saved row and the expected value when the edit runs. */
[[nodiscard]] InvestmentTransition
check_request(const ObjectiveProgressOperation& request) noexcept {
    return check_pursuit_item(request.itemHash);
}

/** State decides which objectives are incomplete when the edit runs. */
[[nodiscard]] InvestmentTransition
check_request(const ObjectiveCompletionOperation& request) noexcept {
    return check_pursuit_item(request.itemHash);
}

/** @return The operation's request checked against its kind's rules. */
[[nodiscard]] InvestmentTransition
check_operation_request(const InvestmentOperation& operation) noexcept {
    return std::visit([](const auto& request) noexcept { return check_request(request); },
                      operation.request);
}

/** @return Queued when the submission can wait for a poll from a peer bound to its account. */
[[nodiscard]] InvestmentTransition check_submission(const InvestmentOperation& operation) noexcept {
    const InvestmentTransition verdict = check_target(operation.target);
    if (verdict.state != InvestmentReceiptState::queued) {
        return verdict;
    }
    // Every change publishes through a peer bound to the account, so refuse when there is none.
    if (publication_peer(operation.target.accountSoid) == nullptr) {
        return refused_transition("publication_peer");
    }
    return check_operation_request(operation);
}

/** @return False only when allocation fails, in which case nothing is retained. */
[[nodiscard]] bool retain_operation(const RetainedOperation& entry) noexcept {
    try {
        g_operations.push_back(entry);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

} // namespace

/** The request's variant index is its kind, as the static_asserts above require. */
InvestmentOperationKind investment_operation_kind(const InvestmentOperation& operation) noexcept {
    return static_cast<InvestmentOperationKind>(operation.request.index());
}

/** State's `not_ready` means a catalog it needs is still unpublished, so the operation waits. */
InvestmentTransition state_refusal_transition(const char* reason) noexcept {
    if (reason == nullptr) {
        return refused_transition("transaction");
    }
    return std::string_view(reason) == "not_ready" ? deferred_transition(InvestmentWait::catalog)
                                                   : refused_transition(reason);
}

/** Checks the target first, so a stale context is reported before any other refusal. */
InvestmentTransition check_investment_operation(const InvestmentOperation& operation) noexcept {
    const InvestmentTransition verdict = check_target(operation.target);
    if (verdict.state != InvestmentReceiptState::queued) {
        return verdict;
    }
    return check_operation_request(operation);
}

/** Operations run in submission order per account, so each edit sees the ones before it. */
bool next_investment_operation_locked(std::uint64_t accountSoid,
                                      InvestmentOperation& operation) noexcept {
    for (const RetainedOperation& entry : g_operations) {
        if (!finished(entry.receipt.state) && entry.operation.target.accountSoid == accountSoid) {
            operation = entry.operation;
            if (auto* grant = std::get_if<GrantItemOperation>(&operation.request)) {
                grant->quantity -= entry.grantedQuantity;
            }
            return true;
        }
    }
    return false;
}

/** Logs only state or wait changes, so waiting or granting in steps does not log every poll. */
void record_investment_transition_locked(std::uint64_t operationId,
                                         const InvestmentTransition& transition) noexcept {
    RetainedOperation* retained = find_operation(operationId);
    if (retained == nullptr || finished(retained->receipt.state)) {
        return;
    }
    const InvestmentTransition merged =
        retained->grantedQuantity != 0 ? continue_grant(transition) : transition;
    retained->grantedQuantity += transition.grantedQuantity;
    const bool changed =
        retained->receipt.state != merged.state || retained->receipt.wait != merged.wait;
    apply_transition(retained->receipt, merged);
    if (changed) {
        report_receipt("execute", retained->receipt);
    }
}

/** Receipts live only in this process, so none survive a game exit. */
void release_investment_operations_locked() noexcept {
    std::vector<RetainedOperation>{}.swap(g_operations);
}

/** Reads the account keys, then the peers bound to them. */
bool investment_context(InvestmentContext& output) noexcept {
    output = {};
    const std::shared_lock lock(session_lock());
    if (!state::account_identity(output.account)) {
        output = {};
        return false;
    }
    const state::AccountIdentity& account = output.account;
    output.live = {account.primarySoid,
                   state::account::selected_character_soid(account),
                   state::build_data::content_hash()};
    output.peerOwnsAccount = publication_peer(account.primarySoid) != nullptr;
    return true;
}

/** A repeated id never runs again and returns the receipt already retained. */
InvestmentSubmission submit_investment_operation(const InvestmentOperation& operation,
                                                 std::uint32_t editorPid,
                                                 InvestmentReceipt& receipt) noexcept {
    const std::lock_guard lock(session_lock());
    if (const RetainedOperation* retained = find_operation(operation.id)) {
        receipt = present(*retained);
        return retained->operation == operation ? InvestmentSubmission::accepted
                                                : InvestmentSubmission::reused;
    }
    RetainedOperation entry{};
    entry.operation = operation;
    entry.editorPid = editorPid;
    entry.receipt.operationId = operation.id;
    entry.receipt.target = operation.target;
    entry.receipt.kind = investment_operation_kind(operation);
    apply_transition(entry.receipt, check_submission(operation));
    if (!retain_operation(entry)) {
        apply_transition(entry.receipt, refused_transition("allocation"));
        report_receipt("submit", entry.receipt);
        receipt = entry.receipt;
        return InvestmentSubmission::refused;
    }
    report_receipt("submit", entry.receipt);
    receipt = present(entry);
    return InvestmentSubmission::accepted;
}

/** Answers from the retained receipt alone, without running anything. */
bool investment_receipt(std::uint64_t operationId, InvestmentReceipt& receipt) noexcept {
    const std::shared_lock lock(session_lock());
    const RetainedOperation* retained = find_operation(operationId);
    receipt = retained != nullptr ? present(*retained) : InvestmentReceipt{};
    return retained != nullptr;
}

/** A pending receipt stays, so the editor can still learn how its operation ends. */
bool acknowledge_investment_receipt(std::uint64_t operationId,
                                    InvestmentReceipt& receipt) noexcept {
    const std::lock_guard lock(session_lock());
    RetainedOperation* retained = find_operation(operationId);
    if (retained == nullptr) {
        receipt = {};
        return false;
    }
    receipt = present(*retained);
    if (!finished(retained->receipt.state)) {
        return false;
    }
    report_receipt("acknowledge", retained->receipt);
    g_operations.erase(g_operations.begin() + (retained - g_operations.data()));
    return true;
}

/** Holds the session lock the operation lane runs under, so nothing refused is mid-execution. */
void refuse_pending_investment_operations(const char* reason) noexcept {
    const std::lock_guard lock(session_lock());
    const InvestmentTransition refusal = refused_transition(reason);
    for (RetainedOperation& entry : g_operations) {
        if (!finished(entry.receipt.state)) {
            apply_transition(entry.receipt,
                             entry.grantedQuantity != 0 ? continue_grant(refusal) : refusal);
            report_receipt("disconnect", entry.receipt);
        }
    }
}

/** A running editor keeps its receipts, so an id it reads as unknown never reached the game. */
void forget_exited_editor_receipts(
    bool (*editorExited)(std::uint32_t editorPid) noexcept) noexcept {
    const std::lock_guard lock(session_lock());
    std::erase_if(g_operations, [editorExited](const RetainedOperation& entry) noexcept {
        return editorExited(entry.editorPid);
    });
}

} // namespace sunrise::server::bap
