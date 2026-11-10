#include <algorithm>
#include <span>

#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

const void* g_actorCommandPolicyContext{};
ActorCommandPolicy g_actorCommandPolicy{};

} // namespace

/**
 * Resolves one durable selector against the current pinned SDK.
 * @param generation Link the squad's body lives on. The policy lane reports provocation with it,
 * and the squad's reports are admitted only from there.
 */
[[nodiscard]] ActorCommandPolicyStatus dispatch_actor_command(const RuntimeInstance& instance,
                                                              const lua_vm::Intent& intent,
                                                              std::uint64_t generation) noexcept {
    if (instance.view.catalog == nullptr) {
        return ActorCommandPolicyStatus::refused;
    }
    const sdk::Snapshot published = sdk::snapshot();
    if (published == nullptr) {
        return ActorCommandPolicyStatus::refused;
    }
    const std::span<const std::byte> sdkBuild = published->sdk_build_sha256();
    if (sdkBuild.size() != intent.sdkBuildSha256.size()
        || !std::equal(sdkBuild.begin(), sdkBuild.end(), intent.sdkBuildSha256.begin())
        || instance.view.catalog->sdk_build_sha256().size() != sdkBuild.size()
        || !std::equal(
            sdkBuild.begin(), sdkBuild.end(), instance.view.catalog->sdk_build_sha256().begin())) {
        return ActorCommandPolicyStatus::refused;
    }
    const auto squads = published->squads();
    if (intent.firstRow >= squads.size()
        || !format::squad_runnable(squads[intent.firstRow].flags)) {
        return ActorCommandPolicyStatus::refused;
    }
    const auto commands = published->actor_command_definitions();
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const format::ActorCommandDefinition& command = commands[index];
        if (command.selector != intent.actorCommandSelector) {
            continue;
        }
        const bool valueValid = command.effect == format::ActorCommandEffect::setFaction
                                && (intent.actorCommandValue == command.factionNone
                                    || intent.actorCommandValue == command.factionRemoved
                                    || intent.actorCommandValue == command.factionHostileToAll);
        if (command.payloadHandle == 0
            || command.provenance != format::ActorSemanticProvenance::executableStatic
            || command.flags != format::kActorCommandDefinitionExact || !valueValid) {
            return ActorCommandPolicyStatus::refused;
        }
        if (g_actorCommandPolicy == nullptr) {
            return ActorCommandPolicyStatus::unavailable;
        }
        const ActorCommandPolicyRequest request{
            .binding = instance.view.binding,
            .sdkBuildSha256 = intent.sdkBuildSha256,
            .activityClientGeneration = generation,
            .squadRow = intent.firstRow,
            .commandRow = static_cast<std::uint32_t>(index),
            .commandSelector = command.selector,
            .value = intent.actorCommandValue,
        };
        return g_actorCommandPolicy(g_actorCommandPolicyContext, request);
    }
    return ActorCommandPolicyStatus::refused;
}

/** Installs the gameplay policy seam. */
void install_actor_command_policy(const void* context, ActorCommandPolicy policy) noexcept {
    g_actorCommandPolicyContext = context;
    g_actorCommandPolicy = policy;
}

} // namespace sunrise::server::activity::mission
