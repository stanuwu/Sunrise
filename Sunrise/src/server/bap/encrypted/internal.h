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
    /** A Triumph claim changed the account flag bank and its image has to follow. */
    bool hasRecordClaim{};
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

/**
 * Encodes, seals, and frames one correlated status-200 reply.
 * @param scratch Lock-owned transform buffers.
 * @param route Service route data naming the response service.
 * @param taskId Request correlation id to echo.
 * @param key Active AES-GCM session key.
 * @param nonce Send-direction nonce for this reply.
 * @param body Encoded response body, which is empty when its codec refused.
 * @param framedSize Receives the complete outer-frame size, or zero on failure.
 * @return True when the payload, seal, and outer frame all fit.
 */
[[nodiscard]] bool encode(Scratch& scratch,
                          const ServiceRoute& route,
                          std::uint32_t taskId,
                          std::span<const std::byte, state::kAesKeySize> key,
                          std::span<const std::byte, state::kBapNonceSize> nonce,
                          std::span<const std::byte> body,
                          std::size_t& framedSize) noexcept;

} // namespace reply

/** Owns request-body processing for an encrypted service route. */
namespace body {

/**
 * Processes a request body and encodes a correlated body when the route needs one.
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

/**
 * Appends the queuez snapshots one subscription needs, including the Family-4 companion.
 * A snapshot that cannot be built is reported and skipped. The subscribe is answered either way,
 * because a request left without a response kills the link on the Client's missing-recipient path.
 * @param scratch Lock-owned transform buffers.
 * @param before Queuez state visible to the current BAP peer.
 * @param subscription Family the Client picked.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced once per appended frame.
 * @param response Caller-owned output containing the existing response prefix.
 * @param written Existing byte count, updated after each complete push is appended.
 * @param after Receives the queuez state published after caller output is copied.
 * @param armsRepush Receives whether the Family-4 companion needs its delayed second copy.
 * @param armsBannerRepush Receives whether a family-zero body needs its delayed second copy.
 */
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

/** Appends one next-version full Family-4 snapshot used to resynchronize another peer. */
[[nodiscard]] bool
append_account_resync_notification(Scratch& scratch,
                                   const queuez::SessionState& before,
                                   std::span<const std::byte, state::kAesKeySize> key,
                                   std::array<std::byte, state::kBapNonceSize>& nonce,
                                   std::span<std::byte> response,
                                   std::size_t& written,
                                   queuez::SessionState& after) noexcept;

/**
 * Appends the family-zero banner pair as its own notification.
 * Sent twice per boot at the same version, which is what survives the state-1 DECLARED race.
 * `after` records the delivery, or a later 504 move cannot name the record it must release.
 * @param scratch Lock-owned transform buffers.
 * @param before Queuez state the pair is delivered against.
 * @param familyRootSoid Root the Client subscribed for Family 3.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced only by a complete frame.
 * @param response Caller-owned output containing prior frames.
 * @param written Existing byte count, updated by a complete frame.
 * @param after Receives the state carrying the recorded delivery.
 * @return True when the banner frame is appended.
 */
[[nodiscard]] bool append_banner_notification(Scratch& scratch,
                                              const queuez::SessionState& before,
                                              std::uint64_t familyRootSoid,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              queuez::SessionState& after) noexcept;

/**
 * Appends the family-zero pair that follows an opcode-504 pick.
 * The Client holds the objIdx-1 buffer for one character at a time, so the pair moves with it. A
 * pick naming the character it already holds republishes the pair in place.
 * @param before Queuez state after the family-four move.
 * @param selectedCharacter Character the pick named.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced only by a complete frame.
 * @param response Caller-owned output containing prior frames.
 * @param written Existing byte count, updated by a complete frame.
 * @param after Receives the state published once the frame is copied.
 * @return True when a frame went out and `after` carries the advanced ladder.
 */
[[nodiscard]] bool
append_banner_move_notification(Scratch& scratch,
                                const queuez::SessionState& before,
                                std::uint64_t selectedCharacter,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                queuez::SessionState& after) noexcept;

/**
 * Appends the fixed opcode-505 Family-4 selection patch.
 * @param scratch Lock-owned transform buffers.
 * @param change Staged queuez after-image and account definition.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce after the correlated svc-11 response.
 * @param response Caller-owned output containing the existing response prefix.
 * @param written Existing byte count, updated after the complete push is appended.
 * @return True when the exact 17-byte patch and complete svc-123 frame fit.
 */
[[nodiscard]] bool
append_change_character_notification(Scratch& scratch,
                                     const queuez::ChangeCharacter& change,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept;

/**
 * Appends the opcode-504 Family-4 move to the picked character.
 * @param scratch Lock-owned transform buffers.
 * @param select Staged after-image, object definitions, and both character keys.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce after the correlated svc-11 response.
 * @param response Caller-owned output containing the existing response prefix.
 * @param written Existing byte count, updated after the complete push is appended.
 * @return True when the 3 object operations and the whole svc-123 frame fit.
 */
[[nodiscard]] bool
append_select_character_notification(Scratch& scratch,
                                     const queuez::SelectCharacter& select,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept;

/** Appends the opcode-403 Family-4 character upsert that exposes the equipped item swap. */
[[nodiscard]] bool
append_equipment_swap_notification(Scratch& scratch,
                                   const queuez::EquipmentSwap& swap,
                                   const state::PendingEquipmentSwap& mutation,
                                   std::span<const std::byte, state::kAesKeySize> key,
                                   std::span<const std::byte, state::kBapNonceSize> nonce,
                                   std::span<std::byte> response,
                                   std::size_t& written) noexcept;

/** Appends the opcode-406 Family-4 character upsert carrying changed inventory-row flags. */
[[nodiscard]] bool
append_item_state_notification(Scratch& scratch,
                               const queuez::EquipmentSwap& update,
                               const state::PendingItemState& mutation,
                               std::span<const std::byte, state::kAesKeySize> key,
                               std::span<const std::byte, state::kBapNonceSize> nonce,
                               std::span<std::byte> response,
                               std::size_t& written) noexcept;

/**
 * Appends the same-character Family-0 appearance upsert paired with one equipment swap.
 * The
 * update owns its nonce advance only after the complete notification fits.
 */
[[nodiscard]] bool
append_equipment_appearance_refresh_notification(Scratch& scratch,
                                                 const queuez::CharacterAppearanceRefresh& refresh,
                                                 const state::PendingEquipmentSwap& mutation,
                                                 std::span<const std::byte, state::kAesKeySize> key,
                                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                                 std::span<std::byte> response,
                                                 std::size_t& written) noexcept;

/** Appends the Family-0 refresh owed by a socket change on an equipped item. */
[[nodiscard]] bool
append_socket_appearance_refresh_notification(Scratch& scratch,
                                              const queuez::CharacterAppearanceRefresh& refresh,
                                              const state::PendingSocketPlug& mutation,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written) noexcept;

/** Appends the Family-0 character ability refresh owed by a subclass selection. */
[[nodiscard]] bool
append_subclass_appearance_refresh_notification(Scratch& scratch,
                                                const queuez::CharacterAppearanceRefresh& refresh,
                                                const state::PendingSubclassSelection& mutation,
                                                std::span<const std::byte, state::kAesKeySize> key,
                                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                                std::span<std::byte> response,
                                                std::size_t& written) noexcept;

/** Appends a Family-3 character record followed by the changed account roster after equip. */
[[nodiscard]] bool
append_equipment_roster_refresh_notification(Scratch& scratch,
                                             const queuez::RosterAppearanceRefresh& refresh,
                                             const state::PendingEquipmentSwap& mutation,
                                             std::span<const std::byte, state::kAesKeySize> key,
                                             std::array<std::byte, state::kBapNonceSize>& nonce,
                                             std::span<std::byte> response,
                                             std::size_t& written) noexcept;

/** Appends a Family-3 character-only appearance refresh after an equipped socket change. */
[[nodiscard]] bool
append_socket_roster_refresh_notification(Scratch& scratch,
                                          const queuez::RosterAppearanceRefresh& refresh,
                                          const state::PendingSocketPlug& mutation,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::array<std::byte, state::kBapNonceSize>& nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept;

/** Appends a Family-3 character-only appearance refresh after a subclass selection. */
[[nodiscard]] bool
append_subclass_roster_refresh_notification(Scratch& scratch,
                                            const queuez::RosterAppearanceRefresh& refresh,
                                            const state::PendingSubclassSelection& mutation,
                                            std::span<const std::byte, state::kAesKeySize> key,
                                            std::array<std::byte, state::kBapNonceSize>& nonce,
                                            std::span<std::byte> response,
                                            std::size_t& written) noexcept;

/** Refreshes the selected character's complete Family-0 appearance from committed State. */
[[nodiscard]] bool
append_account_resync_appearance_notification(Scratch& scratch,
                                              const queuez::SessionState& before,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              queuez::SessionState& after) noexcept;

/** Refreshes the selected character and account roster from committed State. */
[[nodiscard]] bool
append_account_resync_roster_notification(Scratch& scratch,
                                          const queuez::SessionState& before,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::array<std::byte, state::kBapNonceSize>& nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          queuez::SessionState& after) noexcept;

/** Appends the opcode-903 Family-4 item-instance upsert exposing one socket selection. */
[[nodiscard]] bool
append_socket_plug_notification(Scratch& scratch,
                                const queuez::SocketPlug& socketPlug,
                                const state::PendingSocketPlug& mutation,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::span<const std::byte, state::kBapNonceSize> nonce,
                                std::span<std::byte> response,
                                std::size_t& written) noexcept;

/** Appends the opcode-801 Family-4 subclass item-instance upsert. */
[[nodiscard]] bool
append_subclass_selection_notification(Scratch& scratch,
                                       const queuez::SubclassSelection& selection,
                                       const state::PendingSubclassSelection& mutation,
                                       std::span<const std::byte, state::kAesKeySize> key,
                                       std::span<const std::byte, state::kBapNonceSize> nonce,
                                       std::span<std::byte> response,
                                       std::size_t& written) noexcept;

/** Appends a Family-4 character upsert plus newly acquired item-instance upsert. */
[[nodiscard]] bool
append_item_acquisition_notification(Scratch& scratch,
                                     const queuez::ItemAcquisition& acquisition,
                                     const state::PendingItemAcquisition& mutation,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::span<const std::byte, state::kBapNonceSize> nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written) noexcept;

/** Appends one full Family-4 account upsert for a profile-stack acquisition. */
[[nodiscard]] bool
append_profile_item_acquisition_notification(Scratch& scratch,
                                             const queuez::ProfileItemAcquisition& acquisition,
                                             const state::PendingProfileItemAcquisition& mutation,
                                             std::span<const std::byte, state::kAesKeySize> key,
                                             std::span<const std::byte, state::kBapNonceSize> nonce,
                                             std::span<std::byte> response,
                                             std::size_t& written) noexcept;

/** Appends a Family-4 character upsert followed by one empty item-instance release. */
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
