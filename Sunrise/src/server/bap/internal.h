#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../client/network/consumer.h"
#include "../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../middleware/bap/frame.h"
#include "../../state/activity/bubble_authority/definition.h"
#include "../../state/activity/definition.h"
#include "../../state/build_data/scenarios/definition.h"
#include "../../state/runtime/state.h"
#include "encrypted/queuez/definition.h"

namespace sunrise::server::bap {

/** One session per transport peer slot, so a connection id indexes this array directly. */
inline constexpr std::size_t kSessionCount = client::network::kBapConnectionCount;
/** A delivered activity frame defers the next silence-prevention write by five seconds. */
inline constexpr std::uint64_t kActivityKeepaliveIntervalMs = 5'000;

/** Fixed scratch storage owned by the lock, kept off the Client thread's stack. */
struct Scratch {
    std::array<std::byte, client::network::kBapFrameCapacity> plaintext{};
    std::array<std::byte, client::network::kBapFrameCapacity> responseBody{};
    std::array<std::byte, client::network::kBapFrameCapacity> responsePayload{};
    std::array<std::byte, client::network::kBapFrameCapacity> sealed{};
    std::array<std::byte, client::network::kBapFrameCapacity> framed{};
    /** Roster groups the outbound body's slot spans point into, top-level and per-bubble alike. */
    std::array<state::build_data::scenarios::RosterGroup,
               middleware::bap::activity_message::sensor_auth_update::kGroupCapacity>
        rosterGroups{};
    /** Per-bubble sub-blocks the outbound body's field-1 span points into. */
    std::array<middleware::bap::activity_message::sensor_auth_update::BubbleSubBlock,
               state::build_data::scenarios::kBubbleCapacity>
        rosterSubBlocks{};
    /** Keys each sub-block carries, which its own span points into. */
    std::array<
        std::array<std::uint32_t, state::build_data::scenarios::kDestinationBubbleGroupCapacity>,
        state::build_data::scenarios::kBubbleCapacity>
        rosterSubBlockKeys{};
};

/**
 * What one staged roster body owes State, and the counters to put back if it is discarded.
 * A bubble is offered once, and the state byte rebuilds every object the roster owns. Both may
 * move only once the frame reaches the caller.
 */
struct RosterPublication {
    state::activity::bubble_authority::Grant grant{};
    /** ActivityClient generation that staged this grant and its roster counters. */
    std::uint64_t bindingGeneration{};
    std::uint32_t priorGroups{};
    std::uint8_t priorSends{};
    std::uint8_t priorState{};
    /** Set when the staged body carried a bubble grant that State has not recorded yet. */
    bool hasGrant{};
    /** Set while a roster body is staged and its outcome is undecided. */
    bool staged{};
};

/** ActivityClient role owned by one authenticated BAP link. */
enum class ActivityClientRole : std::uint8_t {
    none,
    privateCurrent,
    publicTarget,
};

/** Exact activity-session generations owned by one BAP link. */
struct ActivityClientBinding {
    /** Target/current session that every activity envelope on this link names. */
    state::activity::SessionBinding session{};
    /** Same as session for private links; advertised source for public targets. */
    state::activity::SessionBinding source{};
    std::uint64_t groupSessionId{};
    std::uint64_t hostGeneration{};
    /** Changes on every bind and rejoin, even when the session id stays the same. */
    std::uint64_t bindingGeneration{};
    /** Private: last citizen region. Public: immutable region captured by the host binding. */
    std::int32_t advertisedRegion{-1};
    ActivityClientRole role{ActivityClientRole::none};
};

/** Patch epoch tied to the exact ActivityClient binding that received it. */
struct BoundPatchEpoch {
    middleware::bap::activity_message::patch_epoch::PatchEpoch value{};
    std::uint64_t bindingGeneration{};
    bool seen{};
};

/** Host-session retain staged by one membership body until its frame is published. */
struct AdvertisementPublication {
    std::uint64_t hostGeneration{};
    bool staged{};
};

/** Mutable transport state owned by one BAP connection. */
struct Session {
    std::uint32_t id{};
    bool authenticated{};
    std::array<std::byte, state::kBapNonceSize> sendNonce{};
    std::array<std::byte, state::kBapNonceSize> receiveNonce{};
    /** Opaque State handle taken only after the server hello authenticates. */
    state::matchmaking::ContextHandle matchmakingContext{};
    /** Exact private or public ActivityClient generation owned by this connection. */
    ActivityClientBinding activity{};
    /** Tick count after which the activity link owes its next keepalive write. */
    std::uint64_t activityKeepaliveDueTick{};
    /** Client member key from the join request. It seeds the membership id. */
    std::uint64_t activityMemberKey{};
    /**
     * Character the join request named, or zero when it carried none.
     * The roster's participation key must be the character the client signed in on. The client
     * binds its player by matching that value.
     */
    std::uint64_t activityCharacterSoid{};
    /** Tick count after which the activity link owes its next roster update. */
    std::uint64_t activityRosterDueTick{};
    /**
     * Binding generation whose membership body this link has already delivered.
     * The client sets its membership flag once and never clears it, and never acknowledges a body
     * on a public-target link, so this is a one-shot per binding. Latched on delivery, not encode.
     */
    std::uint64_t activityMembershipSentGeneration{};
    /**
     * Tick count until which the client is loading, so the roster runs at its faster cadence.
     * A join and a transition-token change are the only two things that open it.
     */
    std::uint64_t activityTransitionUntilTick{};
    /** The client's own patch epoch, scoped to the binding that received message 52. */
    BoundPatchEpoch activityPatchEpoch{};
    /** Group set the last roster update published, folded into one comparable value. */
    std::uint32_t activityRosterGroups{};
    /** Roster updates sent on this connection, capped once the warm-up bumps are spent. */
    std::uint8_t activityRosterSends{};
    /** Per-entry state byte the last roster update carried. */
    std::uint8_t activityRosterState{};
    /** Host row retained by the last delivered citizen advertisement. */
    std::uint64_t activityAdvertisementHostGeneration{};
    /** Host row retained by a staged membership body until publication is known. */
    AdvertisementPublication activityAdvertisementStaged{};
    /**
     * Reason code of the last logged roster outcome.
     * The push runs every second, so a refusal is logged only when the reason changes. One flag
     * for every reason hides the second failure behind the first.
     */
    std::uint8_t activityRosterReason{};
    /** What one staged roster body owes, and what to put back if it never reaches the caller. */
    RosterPublication activityRosterStaged{};
    /** Queuez versions and residents published only through this authenticated peer. */
    encrypted::queuez::SessionState queuez{};
    /** Tick count after which the owed Family-4 re-push may go out. */
    std::uint64_t family4RepushDueTick{};
    /** Root the owed re-push must use. */
    std::uint64_t family4RepushRoot{};
    /** True while one Family-4 re-push is still owed to this peer. */
    bool family4RepushArmed{};
    /** Tick count after which the owed banner re-push may go out. */
    std::uint64_t bannerRepushDueTick{};
    /** Root the owed banner re-push must use. */
    std::uint64_t bannerRepushRoot{};
    /** True while one banner re-push is still owed to this peer. */
    bool bannerRepushArmed{};
    /** Tick count after which the owed social-roster re-push may go out. */
    std::uint64_t socialRosterRepushDueTick{};
    /** Root the last family-two subscribe was answered against, and the re-push must reuse. */
    std::uint64_t socialRosterRepushRoot{};
    /** True while one family-two re-push is still owed to this peer. */
    bool socialRosterRepushArmed{};
    /** Latest shared-account generation this peer has received. */
    std::uint64_t accountGeneration{};
    /** Newest shared-account generation owed as a full cross-peer refresh. */
    std::uint64_t accountResyncGeneration{};
    /** Set by encrypted processing only after one account mutation commits and is copied out. */
    bool accountMutationPublished{};
    /** True while another peer's account mutation still needs a full local refresh. */
    bool accountResyncArmed{};
    /**
     * Tick count after which the owed ability-icon refresh may go out. A subclass selection
     * invalidates the published ability buckets and the rebuild runs off the Client
     * content-extraction pump, so the inline refresh can carry empty ones; this one re-derives.
     */
    std::uint64_t abilityRefreshDueTick{};
    /** True while one ability-icon refresh is still owed to this peer. */
    bool abilityRefreshArmed{};
};

namespace plaintext {

/**
 * Handles plaintext bootstrap services, arms encryption after service 25, and routes the rest.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param outer Parsed outer frame carrying the service id and its body.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when the service owes no reply, or its response is encoded.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

} // namespace plaintext

namespace encrypted {

/**
 * Authenticates and routes one encrypted post-bootstrap service frame.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param outer Validated encrypted outer frame.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when routing works, any response fits, State commits and the nonce is published.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Sends the owed Family-4 re-push once its delay has passed.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole Family-4 notification is published.
 */
[[nodiscard]] bool consume_deferred(Session& session,
                                    Scratch& scratch,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    bool& touchesScratch) noexcept;

} // namespace encrypted

} // namespace sunrise::server::bap
