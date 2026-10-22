#pragma once

namespace sunrise::core::settings::server::activation {

/**
 * Gates for the default client-activation work, one per bounded domain.
 * Each domain finishes on its own, so partial work in one cannot make another read as ready.
 */
struct Settings {
    /**
     * The activation coordinator: the transition policy the host applies to a client request to
     * start a different activity. Off, such a request is framed and recorded and answered with
     * nothing.
     */
    bool defaultClientActivation{true};
    /**
     * Membership on a public-target link, so the client binds a world container to it.
     * On by default. Sending it hands the client's player create and destroy source to that
     * link's membership block, and a body that does not carry the local player destroys it.
     */
    bool activityPublicMembership{true};
    /**
     * Registers the client feature name that stops a channel closing when its last owner leaves.
     * Diagnostic only, and off by default. It answers whether that close is on the path to the
     * fast-travel freeze; it is not a fix and comes out once the question is settled.
     */
    bool preventOwnerlessChannelClose{false};
    /**
     * Enables server Activity Host mission scripts, as the bundled settings do. A compact
     * multiplayer document takes this default. Off leaves compiled host policy in control.
     */
    bool missionScripting{true};
};

} // namespace sunrise::core::settings::server::activation
