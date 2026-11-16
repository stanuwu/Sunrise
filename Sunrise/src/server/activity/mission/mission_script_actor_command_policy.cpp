#include <algorithm>
#include <span>

#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

const void* g_actorCommandPolicyContext{};
ActorCommandPolicy g_actorCommandPolicy{};

/** @return True when both digests have the same size and bytes. */
[[nodiscard]] bool same_digest(std::span<const std::byte> left,
                               std::span<const std::byte> right) noexcept {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

} // namespace

/**
 * Checks one squad actor command and hands it to the gameplay policy.
 * The squad row is a row of the bound pack; command rows come from the installed pack.
 * @param instance Mission instance whose bound view names the pack.
 * @param intent Staged command naming the squad row and the command selector.
 * @param generation Link the squad's body lives on. The policy lane reports provocation with it,
 * and the squad's reports are admitted only from there.
 * @return The policy's answer, or refused when a row or digest does not match.
 */
[[nodiscard]] ActorCommandPolicyStatus dispatch_actor_command(const RuntimeInstance& instance,
                                                              const lua_vm::Intent& intent,
                                                              std::uint64_t generation) noexcept {
    const sdk::Catalog* const bound = instance.view.catalog.get();
    const sdk::Snapshot installed = sdk::snapshot();
    if (bound == nullptr || installed == nullptr) {
        return ActorCommandPolicyStatus::refused;
    }
    const std::span<const std::byte> boundBuild = bound->sdk_build_sha256();
    // A mod pack has its own build digest; an installed activity binds the installed pack.
    if (!same_digest(boundBuild, intent.sdkBuildSha256)
        || (!bound->from_mod() && !same_digest(boundBuild, installed->sdk_build_sha256()))) {
        return ActorCommandPolicyStatus::refused;
    }
    const auto squads = bound->squads();
    if (intent.firstRow >= squads.size()
        || !format::squad_runnable(squads[intent.firstRow].flags)) {
        return ActorCommandPolicyStatus::refused;
    }
    const auto commands = installed->actor_command_definitions();
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
