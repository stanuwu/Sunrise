#pragma once

#include "server/gameplay/physics/world/activity_policy.h"

namespace sunrise::server::gameplay::physics::host {

/** Inert policy manifest which grants bounded direct-host generic services. */
class ScriptlessPolicy final : public world::IActivityPolicy {
public:
    /** Replaces the manifest before opening a fresh runner lifetime. */
    void configure(const world::ActivityPolicyManifest& manifest) noexcept;
    [[nodiscard]] world::ActivityPolicyManifest manifest() const noexcept override;
    [[nodiscard]] bool initialize(const world::ActivityPolicyContext& context,
                                  world::HostCommands& commands) noexcept override;
    void pre_tick(const world::PolicyTickContext& context,
                  world::HostCommands& commands) noexcept override;
    void post_tick(const world::PolicyTickContext& context,
                   std::span<const world::CommittedEvent> events,
                   world::HostCommands& commands) noexcept override;
    [[nodiscard]] bool save(world::IPolicyStateWriter& writer) const noexcept override;
    [[nodiscard]] bool load(world::IPolicyStateReader& reader) noexcept override;

private:
    world::ActivityPolicyManifest manifest_{};
};

} // namespace sunrise::server::gameplay::physics::host
