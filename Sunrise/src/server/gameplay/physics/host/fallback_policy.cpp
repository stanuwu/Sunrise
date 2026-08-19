#include "fallback_policy.h"

namespace sunrise::server::gameplay::physics::host {

/** Stores one validated scriptless capability envelope. */
void ScriptlessPolicy::configure(const world::ActivityPolicyManifest& manifest) noexcept {
    manifest_ = manifest;
}

/** Returns the fixed direct-host capability envelope. */
world::ActivityPolicyManifest ScriptlessPolicy::manifest() const noexcept {
    return manifest_;
}

/** Starts without actors, commands, or mission decisions. */
bool ScriptlessPolicy::initialize(const world::ActivityPolicyContext& context,
                                  world::HostCommands& commands) noexcept {
    static_cast<void>(context);
    static_cast<void>(commands);
    return true;
}

/** Leaves the direct-host command queue unchanged. */
void ScriptlessPolicy::pre_tick(const world::PolicyTickContext& context,
                                world::HostCommands& commands) noexcept {
    static_cast<void>(context);
    static_cast<void>(commands);
}

/** Observes no activity-specific outcomes. */
void ScriptlessPolicy::post_tick(const world::PolicyTickContext& context,
                                 std::span<const world::CommittedEvent> events,
                                 world::HostCommands& commands) noexcept {
    static_cast<void>(context);
    static_cast<void>(events);
    static_cast<void>(commands);
}

/** Saves empty scriptless policy state. */
bool ScriptlessPolicy::save(world::IPolicyStateWriter& writer) const noexcept {
    static_cast<void>(writer);
    return true;
}

/** Restores empty scriptless policy state. */
bool ScriptlessPolicy::load(world::IPolicyStateReader& reader) noexcept {
    static_cast<void>(reader);
    return true;
}

} // namespace sunrise::server::gameplay::physics::host
