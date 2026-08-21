#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

#include "../../../middleware/bap/family_unsubscription.h"
#include "../../../middleware/bap/frame.h"
#include "../../../middleware/queuez/subscription.h"
#include "../../../state/runtime/character_creation.h"
#include "../../../state/runtime/character_deletion.h"
#include "../../web_service/web_service_runtime.h"
#include "../internal.h"
#include "activity_message/definition.h"
#include "queuez/definition.h"
#include "transactions/definition.h"

namespace sunrise::server::bap::encrypted {

/** Response-body codecs picked by the authenticated request service. */
enum class BodyCodec : std::uint8_t {
    empty,
    accountTranslationResponse,
    activityHostManagerResponse,
    activityMessageRequest,
    activityHostResponse,
    clientConfigResponse,
    familySubscription,
    familyUnsubscription,
    matchmakingResponse,
    steamCertificate,
    userMessageResponse,
    webService,
};

/** Native character creation retained until its roster/account publication succeeds. */
struct CharacterCreationTransaction {
    state::PendingCharacterCreation pending{};
};

/** Character deletion retained until its response has been staged and persistence can commit. */
struct CharacterDeletionTransaction {
    state::PendingCharacterDeletion pending{};
};

/** Equipment mutation and the exact QueueZ after-image promised by its response. */
struct EquipmentSwapTransaction {
    state::PendingEquipmentSwap pending{};
    queuez::EquipmentSwap update{};
};

/** Socket mutation and the exact QueueZ after-image promised by its response. */
struct SocketPlugTransaction {
    state::PendingSocketPlug pending{};
    queuez::SocketPlug update{};
};

/** Subclass ability selection and the exact QueueZ after-image promised by its response. */
struct SubclassSelectionTransaction {
    state::PendingSubclassSelection pending{};
    queuez::SubclassSelection update{};
};

/** Item-state mutation and the exact QueueZ character after-image promised by its response. */
struct ItemStateTransaction {
    state::PendingItemState pending{};
    queuez::EquipmentSwap update{};
};

/** Character acquisition and its exact QueueZ after-image. */
struct ItemAcquisitionTransaction {
    state::PendingItemAcquisition pending{};
    queuez::ItemAcquisition update{};
};

/** Profile acquisition and its exact account/resident QueueZ after-image. */
struct ProfileItemAcquisitionTransaction {
    state::PendingProfileItemAcquisition pending{};
    queuez::ProfileItemAcquisition update{};
};

/** Dismantle mutation and its exact QueueZ after-image. */
struct ItemDismantleTransaction {
    state::PendingItemDismantle pending{};
    queuez::ItemDismantle update{};
};

/** Optional side effect produced while decoding one authenticated service body. */
struct ServiceOutcome {
    bool hasSubscription{};
    middleware::queuez::Subscription subscription{};
    bool hasUnsubscription{};
    middleware::bap::family_unsubscription::Request unsubscription{};
    bool hasChangeCharacter{};
    queuez::ChangeCharacter changeCharacter{};
    bool hasSelectCharacter{};
    queuez::SelectCharacter selectCharacter{};
    /** One service owns at most one independently versioned transaction. */
    using Transaction = std::variant<std::monostate,
                                     state::activity::PendingAllocation,
                                     activity_message::ActivityPlan,
                                     state::matchmaking::PendingMutation,
                                     CharacterCreationTransaction,
                                     CharacterDeletionTransaction,
                                     EquipmentSwapTransaction,
                                     SubclassSelectionTransaction,
                                     SocketPlugTransaction,
                                     ItemStateTransaction,
                                     ItemAcquisitionTransaction,
                                     ProfileItemAcquisitionTransaction,
                                     ItemDismantleTransaction>;
    Transaction transaction{};
};

/** @return The service transaction of the requested type, or null for another route. */
template <typename Transaction>
[[nodiscard]] Transaction* transaction_if(ServiceOutcome& outcome) noexcept {
    return std::get_if<Transaction>(&outcome.transaction);
}

/** @return The service transaction of the requested type, or null for another route. */
template <typename Transaction>
[[nodiscard]] const Transaction* transaction_if(const ServiceOutcome& outcome) noexcept {
    return std::get_if<Transaction>(&outcome.transaction);
}

/** Outbound delivery behavior picked for one authenticated request service. */
enum class ResponseMode : std::uint8_t {
    none,
    reply,
    /** Processes a request body and may emit notifications without a status response. */
    uncorrelatedPush,
};

/** Static response metadata for one supported encrypted request service. */
struct ServiceRoute {
    ResponseMode responseMode{};
    middleware::bap::ResponseService response{};
    BodyCodec bodyCodec{};
    std::string_view successEvent{};
};

/** Owns encrypted service-to-response routing. */
namespace routing {

[[nodiscard]] bool resolve(std::uint16_t request, ServiceRoute& route) noexcept;

} // namespace routing

/** Owns failure reporting for encrypted requests. */
namespace diagnostics {

void report_failure(std::uint16_t service, std::string_view stage) noexcept;

} // namespace diagnostics

/** Owns correlated reply construction for one authenticated request. */
namespace reply {

[[nodiscard]] bool encode(Scratch& scratch,
                          const ServiceRoute& route,
                          std::uint32_t taskId,
                          std::span<const std::byte, state::kAesKeySize> key,
                          std::span<const std::byte, state::kBapNonceSize> nonce,
                          std::span<const std::byte> body,
                          std::size_t& framedSize) noexcept;

} // namespace reply

/** Owns request-body processing for one encrypted service route. */
namespace body {

[[nodiscard]] bool process(const ServiceRoute& route,
                           const queuez::SessionState& queuezState,
                           const ActivityClientBinding& activity,
                           state::matchmaking::ContextHandle matchmakingContext,
                           std::span<const std::byte> requestBody,
                           std::span<std::byte> output,
                           std::size_t& written,
                           ServiceOutcome& outcome) noexcept;

} // namespace body

/** Owns server-initiated encrypted frames appended after correlated replies. */
namespace push {

void append_queuez_notification(Scratch& scratch,
                                const queuez::SessionState& before,
                                const middleware::queuez::Subscription& subscription,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                queuez::SessionState& after,
                                bool& armsRepush,
                                bool& armsBannerRepush) noexcept;

[[nodiscard]] bool
append_account_resync_notification(Scratch& scratch,
                                   const queuez::SessionState& before,
                                   std::span<const std::byte, state::kAesKeySize> key,
                                   std::array<std::byte, state::kBapNonceSize>& nonce,
                                   std::span<std::byte> response,
                                   std::size_t& written,
                                   queuez::SessionState& after) noexcept;

[[nodiscard]] bool append_banner_notification(Scratch& scratch,
                                              const queuez::SessionState& before,
                                              std::uint64_t familyRootSoid,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              queuez::SessionState& after) noexcept;

[[nodiscard]] bool
append_banner_move_notification(Scratch& scratch,
                                const queuez::SessionState& before,
                                std::uint64_t selectedCharacter,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                queuez::SessionState& after) noexcept;

[[nodiscard]] bool
append_change_character_notification(Scratch& scratch,
                                     const queuez::ChangeCharacter& change,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept;

[[nodiscard]] bool
append_select_character_notification(Scratch& scratch,
                                     const queuez::SelectCharacter& select,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept;

[[nodiscard]] bool
append_equipment_swap_notification(Scratch& scratch,
                                   const queuez::EquipmentSwap& swap,
                                   const state::PendingEquipmentSwap& mutation,
                                   std::span<const std::byte, state::kAesKeySize> key,
                                   std::span<const std::byte, state::kBapNonceSize> nonce,
                                   std::span<std::byte> response,
                                   std::size_t& written) noexcept;

[[nodiscard]] bool
append_item_state_notification(Scratch& scratch,
                               const queuez::EquipmentSwap& update,
                               const state::PendingItemState& mutation,
                               std::span<const std::byte, state::kAesKeySize> key,
                               std::span<const std::byte, state::kBapNonceSize> nonce,
                               std::span<std::byte> response,
                               std::size_t& written) noexcept;

[[nodiscard]] bool
append_equipment_appearance_refresh_notification(Scratch& scratch,
                                                 const queuez::CharacterAppearanceRefresh& refresh,
                                                 const state::PendingEquipmentSwap& mutation,
                                                 std::span<const std::byte, state::kAesKeySize> key,
                                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                                 std::span<std::byte> response,
                                                 std::size_t& written) noexcept;

[[nodiscard]] bool
append_socket_appearance_refresh_notification(Scratch& scratch,
                                              const queuez::CharacterAppearanceRefresh& refresh,
                                              const state::PendingSocketPlug& mutation,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written) noexcept;

[[nodiscard]] bool
append_subclass_appearance_refresh_notification(Scratch& scratch,
                                                const queuez::CharacterAppearanceRefresh& refresh,
                                                const state::PendingSubclassSelection& mutation,
                                                std::span<const std::byte, state::kAesKeySize> key,
                                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                                std::span<std::byte> response,
                                                std::size_t& written) noexcept;

[[nodiscard]] bool
append_equipment_roster_refresh_notification(Scratch& scratch,
                                             const queuez::RosterAppearanceRefresh& refresh,
                                             const state::PendingEquipmentSwap& mutation,
                                             std::span<const std::byte, state::kAesKeySize> key,
                                             std::array<std::byte, state::kBapNonceSize>& nonce,
                                             std::span<std::byte> response,
                                             std::size_t& written) noexcept;

[[nodiscard]] bool
append_socket_roster_refresh_notification(Scratch& scratch,
                                          const queuez::RosterAppearanceRefresh& refresh,
                                          const state::PendingSocketPlug& mutation,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::array<std::byte, state::kBapNonceSize>& nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept;

[[nodiscard]] bool
append_subclass_roster_refresh_notification(Scratch& scratch,
                                            const queuez::RosterAppearanceRefresh& refresh,
                                            const state::PendingSubclassSelection& mutation,
                                            std::span<const std::byte, state::kAesKeySize> key,
                                            std::array<std::byte, state::kBapNonceSize>& nonce,
                                            std::span<std::byte> response,
                                            std::size_t& written) noexcept;

[[nodiscard]] bool
append_account_resync_appearance_notification(Scratch& scratch,
                                              const queuez::SessionState& before,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              queuez::SessionState& after) noexcept;

[[nodiscard]] bool
append_account_resync_roster_notification(Scratch& scratch,
                                          const queuez::SessionState& before,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::array<std::byte, state::kBapNonceSize>& nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          queuez::SessionState& after) noexcept;

[[nodiscard]] bool
append_socket_plug_notification(Scratch& scratch,
                                const queuez::SocketPlug& socketPlug,
                                const state::PendingSocketPlug& mutation,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::span<const std::byte, state::kBapNonceSize> nonce,
                                std::span<std::byte> response,
                                std::size_t& written) noexcept;

[[nodiscard]] bool
append_subclass_selection_notification(Scratch& scratch,
                                       const queuez::SubclassSelection& selection,
                                       const state::PendingSubclassSelection& mutation,
                                       std::span<const std::byte, state::kAesKeySize> key,
                                       std::span<const std::byte, state::kBapNonceSize> nonce,
                                       std::span<std::byte> response,
                                       std::size_t& written) noexcept;

[[nodiscard]] bool
append_item_acquisition_notification(Scratch& scratch,
                                     const queuez::ItemAcquisition& acquisition,
                                     const state::PendingItemAcquisition& mutation,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept;

[[nodiscard]] bool
append_profile_item_acquisition_notification(Scratch& scratch,
                                             const queuez::ProfileItemAcquisition& acquisition,
                                             const state::PendingProfileItemAcquisition& mutation,
                                             std::span<const std::byte, state::kAesKeySize> key,
                                             std::span<const std::byte, state::kBapNonceSize> nonce,
                                             std::span<std::byte> response,
                                             std::size_t& written) noexcept;

[[nodiscard]] bool
append_item_dismantle_notification(Scratch& scratch,
                                   const queuez::ItemDismantle& dismantle,
                                   const state::PendingItemDismantle& mutation,
                                   std::span<const std::byte, state::kAesKeySize> key,
                                   std::span<const std::byte, state::kBapNonceSize> nonce,
                                   std::span<std::byte> response,
                                   std::size_t& written) noexcept;

} // namespace push

} // namespace sunrise::server::bap::encrypted
