#pragma once

#include <cstdint>
#include <variant>

#include "../../state/account/account_state.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::bap {

/** The account, selected character and package content an operation was submitted against. */
struct InvestmentTarget {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t contentHash{};

    bool operator==(const InvestmentTarget&) const = default;
};

/** Grants one installed item through the same staging that grants world rewards. */
struct GrantItemOperation {
    std::uint32_t definitionHash{};
    std::uint32_t quantity{};

    bool operator==(const GrantItemOperation&) const = default;
};

/** Replaces the seasonal XP total, provided it still equals the total the editor read. */
struct SeasonalExperienceOperation {
    std::int32_t expected{};
    std::int32_t replacement{};

    bool operator==(const SeasonalExperienceOperation&) const = default;
};

/** Claims one Season pass row through the server's opcode-2400 claim path. */
struct SeasonPassClaimOperation {
    std::uint16_t rewardIndex{};
    std::uint32_t itemHash{};

    bool operator==(const SeasonPassClaimOperation&) const = default;
};

/** Clears the claim flag of one claimed Season pass row. */
struct SeasonPassResetOperation {
    std::uint16_t rewardIndex{};
    std::uint32_t itemHash{};

    bool operator==(const SeasonPassResetOperation&) const = default;
};

/** Replaces the saved flag or value row that one objective of a held pursuit reads. */
struct ObjectiveProgressOperation {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t mutationSerial{};
    std::uint32_t objectiveHash{};
    std::uint8_t ordinal{};
    std::int32_t expected{};
    std::int32_t value{};

    bool operator==(const ObjectiveProgressOperation&) const = default;
};

/** Sets every required objective of a held pursuit that is still incomplete to its target. */
struct ObjectiveCompletionOperation {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t mutationSerial{};

    bool operator==(const ObjectiveCompletionOperation&) const = default;
};

/** What one operation changes. The order matches InvestmentRequest's alternatives. */
enum class InvestmentOperationKind : std::uint8_t {
    grantItem,
    setQuantity,
    setSeasonalExperience,
    claimSeasonPassReward,
    resetSeasonPassClaim,
    setPursuitObjective,
    completePursuitObjectives,
};

/** One operation's request. Each alternative's index is its InvestmentOperationKind. */
using InvestmentRequest = std::variant<GrantItemOperation,
                                       state::QuantityEdit,
                                       SeasonalExperienceOperation,
                                       SeasonPassClaimOperation,
                                       SeasonPassResetOperation,
                                       ObjectiveProgressOperation,
                                       ObjectiveCompletionOperation>;

/** One editor operation, copied out of the editor mapping before it is submitted. */
struct InvestmentOperation {
    /** Chosen by the editor and nonzero. Resubmitting the same id never runs it twice. */
    std::uint64_t id{};
    InvestmentTarget target{};
    InvestmentRequest request{};

    bool operator==(const InvestmentOperation&) const = default;
};

/** Where one operation stands. Refused, published and committedUnpublished are final. */
enum class InvestmentReceiptState : std::uint8_t {
    queued,
    deferred,
    refused,
    published,
    /**
     * State committed the change but its frame failed, so every Family-4 peer resyncs. A grant
     * ends at that step, and the rest of its quantity is never granted.
     */
    committedUnpublished,
};

/** What a queued or deferred operation is waiting for. */
enum class InvestmentWait : std::uint8_t {
    /** No named condition. This covers an in-game presentation the peer is still showing. */
    none,
    publicationPeer,
    accountResync,
    catalog,
};

/** One operation's retained outcome. */
struct InvestmentReceipt {
    std::uint64_t operationId{};
    InvestmentTarget target{};
    InvestmentOperationKind kind{};
    InvestmentReceiptState state{};
    InvestmentWait wait{};
    /**
     * Static label: why the operation was refused, why a committed operation has no frame, or
     * why a published grant stopped short of its full quantity.
     */
    const char* reason{""};
};

/** How the server took one submission. */
enum class InvestmentSubmission : std::uint8_t {
    /**
     * The receipt is retained, and its state says whether the operation was queued or refused.
     * Resubmitting the same request returns the retained receipt without running anything again.
     */
    accepted,
    /** The id was already submitted with a different request. Nothing changed. */
    reused,
    /** Allocation failed, so nothing was retained. */
    refused,
};

/** The live account an editor binds to, and whether a peer can publish to it. */
struct InvestmentContext {
    InvestmentTarget live{};
    state::AccountIdentity account{};
    bool peerOwnsAccount{};
};

/** @return The kind the operation's request names. */
[[nodiscard]] InvestmentOperationKind
investment_operation_kind(const InvestmentOperation& operation) noexcept;

/**
 * Reads the live account under the session lock.
 * @return False when no valid account is loaded.
 */
[[nodiscard]] bool investment_context(InvestmentContext& output) noexcept;

/**
 * Validates an operation without drawing or writing, and retains its receipt until the editor
 * acknowledges it. A refused operation is retained too, unless allocation fails.
 * @param operation Operation with a nonzero identity.
 * @param editorPid Process id of the submitting editor, whose receipts are forgotten once it exits.
 * @param receipt Receives the new receipt, or the one already retained under that identity.
 */
[[nodiscard]] InvestmentSubmission submit_investment_operation(const InvestmentOperation& operation,
                                                               std::uint32_t editorPid,
                                                               InvestmentReceipt& receipt) noexcept;

/** @return False with an empty receipt when no receipt is retained under that identity. */
[[nodiscard]] bool investment_receipt(std::uint64_t operationId,
                                      InvestmentReceipt& receipt) noexcept;

/**
 * Forgets a finished receipt.
 * @param receipt Receives the receipt, or an empty one when none is retained under that identity.
 * @return False while the operation is still pending, or when no receipt is retained.
 */
[[nodiscard]] bool acknowledge_investment_receipt(std::uint64_t operationId,
                                                  InvestmentReceipt& receipt) noexcept;

/**
 * Refuses every queued or deferred operation when the editor link drops, since nobody is left to
 * read how they end. A grant that already published some steps finishes as published with this
 * reason. Finished receipts stay so the editor can read them when it reconnects.
 * @param reason Static label each refused receipt carries.
 */
void refuse_pending_investment_operations(const char* reason) noexcept;

/**
 * Forgets the receipts of every editor process that has exited. Only that process knew their
 * ids, so no one can ask for them again.
 * @param editorExited Returns true once no process runs under the given editor process id.
 */
void forget_exited_editor_receipts(bool (*editorExited)(std::uint32_t editorPid) noexcept) noexcept;

} // namespace sunrise::server::bap
