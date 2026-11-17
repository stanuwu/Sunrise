#pragma once

#include <cstddef>
#include <span>

#include "../../../../gameplay/entity_identities.h"
#include "../../../internal.h"
#include "../../activity_message/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

struct EffectiveRegion;
struct RefreshReport;

/**
 * Appends one `sensor_auth_update` svc9 notification carrying the destination's roster.
 * Nothing is staged before the client's own patch epoch has arrived, because a body carrying a
 * wrong epoch still lands phase 1, skips phase 2, and reports nothing.
 * @param session Connection-owned nonce, activity binding, epoch, and roster counters.
 * @param scratch Lock-owned transform and roster buffers.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @param epoch Epoch from the message being answered, or null to use the connection's own.
 * @param exactRegion Prepared transaction region to use before its State commit, or null.
 * @param solicited True when this answers a client request, which forbids repeat suppression.
 * @param refresh The message-18 refresh this answers, read before its commit, or null.
 * @param allowEntityRetirement True when this body may carry a placed-entity purge.
 * @param peerLeave True to answer activity msg 15: every group retired and nothing granted.
 * @param hostPushOwed Set when a solicited body waits for the host push that carries its owed
 * entity allocation; nothing is staged then.
 * @return True when the roster is built and the type-5 frame encodes atomically.
 */
[[nodiscard]] bool append_roster_notification(
    Session& session,
    Scratch& scratch,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written,
    const middleware::bap::activity_message::patch_epoch::PatchEpoch* epoch = nullptr,
    const EffectiveRegion* exactRegion = nullptr,
    bool solicited = false,
    const RefreshReport* refresh = nullptr,
    bool allowEntityRetirement = true,
    bool peerLeave = false,
    bool* hostPushOwed = nullptr) noexcept;

/**
 * Settles a staged roster body that reached the caller.
 * A bubble is offered once, so its grant is recorded only where the frame is known published.
 * @param session Connection-owned staged roster, cleared here.
 */
void commit_staged_roster(Session& session) noexcept;

/**
 * Puts back what a staged roster body advanced, now that the body has been discarded.
 * The next push must offer the grant again and move the per-entry state byte again.
 * @param session Connection-owned staged roster, cleared here.
 */
void discard_staged_roster(Session& session) noexcept;

/** A claim must still match its captured source before the carrying frames are published. */
[[nodiscard]] bool validate_staged_roster(const Session& session) noexcept;

/** A claim's identities stay fixed until the carrying frames reach caller storage. */
[[nodiscard]] bool begin_staged_roster_publication(
    const Session& session, server::gameplay::entity_identities::PublicationLease& lease) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
