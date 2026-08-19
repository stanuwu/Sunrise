#pragma once

namespace sunrise::core::settings::server::activation {

/**
 * Gates for the default client-activation work, one per bounded domain.
 * Each domain finishes on its own, so partial work in one cannot make another read as ready.
 * With every gate off the host sends the same bytes it did before the domain existed.
 */
struct Settings {
    /**
     * The activation coordinator: the transition policy the host applies to a client request to
     * start a different activity. Off, such a request is framed and recorded and answered with
     * nothing.
     */
    bool defaultClientActivation{true};
    /**
     * Bounded receipt, mirror, and response handling for the activity compatibility traffic.
     * Off, an activity message that changes no state is framed and reported but not recorded, so
     * the arrival registry stays empty.
     */
    bool activityCompatibilityMirror{true};
    /**
     * The external common and channel wrapper on the gameplay packet.
     * TODO: nothing reads this gate. The codec and receive shadow exist; the packet-outcome
     * binding does not. Wire it to the established writer, not before.
     */
    bool gameplayExternalBody{false};
    /**
     * One allowlisted server-owned default entity.
     * TODO: nothing reads this gate. The allocator and channel-2 codec exist offline; a visible
     * class chain and a live outcome path do not. Wire it with those, not before.
     */
    bool serverDefaultEntity{false};
    /**
     * The physics-host bridge: one world opened, ticked and closed per admitted gameplay peer.
     * Off by default because the bridge runs on the game's own render thread, so its cost is a
     * frame stall. It produces no wire output either way.
     */
    bool physicsHostSession{false};
    /**
     * Membership on a public-target link, so the client binds a world container to it.
     * Off by default. Sending it hands the client's player create and destroy source to that
     * link's membership block, and a body that does not carry the local player destroys it.
     */
    bool activityPublicMembership{false};
};

} // namespace sunrise::core::settings::server::activation
