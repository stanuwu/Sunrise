#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/inventory/seen_state.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::web_service {

/** Optional Server action produced while answering one Web Service request. */
struct Outcome {
    bool hasSubscription{};
    middleware::queuez::Subscription subscription{};
    /** A claim changed the account flag bank, so a fresh account image has to follow. */
    bool hasRecordClaim{};
    /** An earned title changed on the selected character; roster and banner must be republished. */
    bool hasTitleEquip{};
    /** An opcode-504 pick was recorded and its Family-4 update still has to follow. */
    bool hasSelectedCharacter{};
    std::uint64_t selectedCharacterSoid{};
    /** An opcode-501 create or 502 delete saved a roster change; the account resync follows. */
    bool hasRosterChange{};
    /** The character an opcode-501 create added, which its reply names. */
    std::uint64_t createdCharacterSoid{};
    /** Reset is precommitted because it changes persistence and account currency together. */
    bool hasArtifactReset{};
    state::ArtifactResetResult artifactReset{};
    /** A WS-701 carried the profile-setup completion marker and State refused it. */
    bool profileSetupRefused{};
    /** A request prepares at most one State mutation and allocates only that exact payload. */
    using Mutation = std::variant<std::monostate,
                                  std::unique_ptr<state::PendingPostmasterClaim>,
                                  std::unique_ptr<state::PendingEquipmentSwap>,
                                  std::unique_ptr<state::PendingSubclassSelection>,
                                  std::unique_ptr<state::PendingItemAcquisition>,
                                  std::unique_ptr<state::PendingProfileItemAcquisition>,
                                  std::unique_ptr<state::PendingItemDismantle>,
                                  std::unique_ptr<state::PendingSocketPlug>,
                                  std::unique_ptr<state::PendingItemState>,
                                  std::unique_ptr<state::PendingArtifactPurchase>,
                                  std::unique_ptr<state::PendingRecordRewardGrant>,
                                  std::unique_ptr<state::PendingSeasonPassReward>,
                                  std::unique_ptr<state::PendingSettingsUpdate>>;
    Mutation mutation{};
};

/** Allocates only the selected mutation outside the request's already deep stack. */
template <typename Mutation, typename... Args>
[[nodiscard]] Mutation* emplace_mutation(Outcome& outcome, Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible_v<Mutation, Args...>);
    outcome.mutation.template emplace<std::monostate>();
    auto storage =
        std::unique_ptr<Mutation>{new (std::nothrow) Mutation(std::forward<Args>(args)...)};
    if (storage == nullptr) {
        return nullptr;
    }
    auto* mutation = storage.get();
    outcome.mutation.template emplace<std::unique_ptr<Mutation>>(std::move(storage));
    return mutation;
}

/** @return The prepared mutation of the requested type, or null when another route ran. */
template <typename Mutation> [[nodiscard]] Mutation* mutation_if(Outcome& outcome) noexcept {
    auto* storage = std::get_if<std::unique_ptr<Mutation>>(&outcome.mutation);
    return storage == nullptr ? nullptr : storage->get();
}

/** @return The prepared mutation of the requested type, or null when another route ran. */
template <typename Mutation>
[[nodiscard]] const Mutation* mutation_if(const Outcome& outcome) noexcept {
    const auto* storage = std::get_if<std::unique_ptr<Mutation>>(&outcome.mutation);
    return storage == nullptr ? nullptr : storage->get();
}

/** Transfers the selected mutation without copying its snapshot. */
template <typename Mutation>
[[nodiscard]] std::unique_ptr<Mutation> take_mutation(Outcome& outcome) noexcept {
    auto* storage = std::get_if<std::unique_ptr<Mutation>>(&outcome.mutation);
    if (storage == nullptr) {
        return {};
    }
    auto mutation = std::move(*storage);
    outcome.mutation.template emplace<std::monostate>();
    return mutation;
}

[[nodiscard]] inline bool has_mutation(const Outcome& outcome) noexcept {
    return outcome.mutation.index() != 0;
}

inline void clear_mutation(Outcome& outcome) noexcept {
    outcome.mutation.template emplace<std::monostate>();
}

/**
 * Issues the next family-5 clock, in Unix seconds.
 * One issuer serves every family-5 publication, so the value the Client extrapolates from only
 * ever moves forward.
 */
[[nodiscard]] std::uint64_t next_family5_clock() noexcept;

/**
 * Answers one request and reports any subscription side effect.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets the prepared action for the caller to publish, and is left empty when
 * the action was refused or the reply could not be encoded.
 * @return False when the envelope header does not parse, or a saved roster change has no
 *         reply; the caller's save transaction must then drop the change.
 */
[[nodiscard]] bool
consume(std::span<const std::byte> request,
        std::span<std::byte> response,
        std::size_t& written,
        Outcome& outcome,
        std::span<const state::account::inventory::PresentedItemRow> presentation = {}) noexcept;

/**
 * Encodes the reply to an opcode-501 create or 502 delete, a refusal when nothing changed.
 * @param message Parsed request; its opcode picks the reply shape.
 * @param family4Version Revision carrying the change, or kNoFamily4Publication.
 * @param outcome The roster change and created id, empty for a refusal.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size.
 * @return False when the reply does not fit.
 */
[[nodiscard]] bool encode_roster_reply(const middleware::web_service::Message& message,
                                       std::int32_t family4Version,
                                       const Outcome& outcome,
                                       std::span<std::byte> response,
                                       std::size_t& written) noexcept;

/** Encodes the normal refusal shape for a request that may publish resident references. */
[[nodiscard]] bool encode_resident_dependent_refusal(std::span<const std::byte> request,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& refused) noexcept;

} // namespace sunrise::server::web_service
