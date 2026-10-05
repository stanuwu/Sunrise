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
#include "../../state/account/inventory/seen_state.h"
#include "../../state/runtime/runtime.h"
#include "../../state/runtime/state_vendor_reputation_runtime.h"

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
    /** Reset is precommitted because it changes persistence and account currency together. */
    bool hasArtifactReset{};
    state::ArtifactResetResult artifactReset{};
    /** A WS-701 carried the profile-setup completion marker and State refused it. */
    bool profileSetupRefused{};
    /** A request prepares at most one State mutation and allocates only that exact payload. */
    using Mutation = std::variant<std::monostate,
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
                                  std::unique_ptr<state::PendingSettingsUpdate>,
                                  std::unique_ptr<state::PendingVendorReputation>>;
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

/** Records the final profile-stack creation reply and exact Family-4 account revision. */
void report_profile_item_acquisition_response(const middleware::web_service::Message& message,
                                              std::int32_t family4Version,
                                              std::uint32_t definitionHash,
                                              std::int32_t quantity,
                                              std::span<const std::byte> response) noexcept;

/** Records a dismantle reply after its exact Family-4 version and removed instance are known. */
void report_item_dismantle_response(const middleware::web_service::Message& message,
                                    std::int32_t family4Version,
                                    std::uint64_t dismantledInstanceSoid,
                                    std::span<const std::byte> response) noexcept;

/** Records a socket-action reply after its exact item-instance Family-4 revision is known. */
void report_socket_plug_response(const middleware::web_service::Message& message,
                                 std::int32_t family4Version,
                                 std::uint64_t targetInstanceSoid,
                                 std::uint8_t socketLane,
                                 std::uint16_t plugDefinitionIndex,
                                 std::span<const std::byte> response) noexcept;

/** Records an opcode-801 reply after its exact subclass item-instance revision is known. */
void report_subclass_selection_response(const middleware::web_service::Message& message,
                                        std::int32_t family4Version,
                                        const state::PendingSubclassSelection& mutation,
                                        std::span<const std::byte> response) noexcept;

/**
 * Re-encodes a prepared reply as a refusal after its Queuez staging failed.
 * The reply was already encoded as a success, and nothing is published now, so it must say so.
 * @param message Parsed request whose correlation fields are echoed.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size.
 * @return True when the refusal fits.
 */
[[nodiscard]] bool encode_staging_refusal(const middleware::web_service::Message& message,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept;

/**
 * Answers one whole supported Web Service request body.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Answers one request and reports any subscription side effect.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets the prepared action for the caller to publish, and is left empty when
 * the action was refused or the reply could not be encoded.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool
consume(std::span<const std::byte> request,
        std::span<std::byte> response,
        std::size_t& written,
        Outcome& outcome,
        std::span<const state::account::inventory::PresentedItemRow> presentation = {}) noexcept;

/** Encodes the normal refusal shape for a request that may publish resident references. */
[[nodiscard]] bool encode_resident_dependent_refusal(std::span<const std::byte> request,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& refused) noexcept;

} // namespace sunrise::server::web_service
