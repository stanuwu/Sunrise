#pragma once

namespace sunrise::core::settings::server::activation {

/**
 * Gates for the default client-activation work, one per bounded domain.
 * Each domain finishes on its own, so partial work in one cannot make another read as ready.
 */
struct Settings {
    /**
     * Registers the client feature name that stops a channel closing when its last owner leaves.
     * Diagnostic only, and off by default. It answers whether that close is on the path to the
     * fast-travel freeze; it is not a fix and comes out once the question is settled.
     */
    bool preventOwnerlessChannelClose{false};
    /** Enables server Activity Host mission scripts. Off leaves compiled host policy in control. */
    bool missionScripting{false};
};

} // namespace sunrise::core::settings::server::activation
