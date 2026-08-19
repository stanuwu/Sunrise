#pragma once

#include <cstdint>

#include "../internal.h"
#include "internal.h"
#include "queuez/queuez_state_validation.h"
#include "transactions/definition.h"

namespace sunrise::server::bap::encrypted {

/** Connection fields one request may publish, captured before its transaction commits. */
struct ConnectionFields {
    middleware::bap::activity_message::patch_epoch::PatchEpoch patchEpoch{};
    /** The join carries the only member key the client ever sends. */
    std::uint64_t joinMemberKey{};
    /** The join also names the character the player signed in on. */
    std::uint64_t joinCharacterSoid{};
    bool retainsPatchEpoch{};
    /** Set by a join or a transition-token change, which are the client starting a load. */
    bool opensTransitionWindow{};
    /** Set by a join alone, which re-arms the roster warm-up the new container needs. */
    bool joinsActivity{};
};

/** Reserves one process-lifetime ActivityClient generation without wrapping. */
[[nodiscard]] bool reserve_activity_binding_generation(std::uint64_t& generation) noexcept;

/**
 * Captures the connection fields one service outcome carries.
 * @param outcome Prepared outcome, still holding its uncommitted mutations.
 * @return The fields to publish once the transaction commits.
 */
[[nodiscard]] ConnectionFields connection_fields(const ServiceOutcome& outcome) noexcept;

/**
 * Publishes the captured connection fields after a successful commit.
 * @param session Connection-owned activity binding and epoch.
 * @param publication Committed State bindings.
 * @param fields Fields captured before the commit.
 */
void publish_connection_fields(Session& session,
                               const transactions::Publication& publication,
                               const ConnectionFields& fields) noexcept;

/** Stages one retained host row until the membership frame is published or discarded. */
void stage_activity_advertisement(Session& session, std::uint64_t hostGeneration) noexcept;

/** Publishes the staged advertisement retain and releases the previous delivered one. */
void commit_staged_advertisement(Session& session) noexcept;

/** Releases a retained advertisement that never reached the caller. */
void discard_staged_advertisement(Session& session) noexcept;

/** Releases every exact activity binding and advertisement owned by one BAP link. */
void release_activity_connection(Session& session) noexcept;

/**
 * Arms the owed Family-4 and banner re-pushes when the queuez publication asks for them.
 * @param session Connection-owned re-push timers.
 * @param queuezPublication Staged queuez publication.
 */
void arm_repushes(Session& session, const queuez::StagedPublication& queuezPublication) noexcept;

} // namespace sunrise::server::bap::encrypted
