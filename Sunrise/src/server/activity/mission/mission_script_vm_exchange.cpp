#include <tuple>

#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm {

using detail::Impl;
using detail::VmAccess;

/** Member swaps retain debug iterator proxies; Lua storage keeps its heap address. */
bool exchange(Vm& first, Vm& second) noexcept {
    Impl& left = VmAccess::get(first);
    Impl& right = VmAccess::get(second);
    if (left.frame != nullptr || right.frame != nullptr) {
        return false;
    }
    const auto members = [](Impl& value) noexcept {
        auto& [attempt,
               deviceRequests,
               deviceRequestSources,
               squadPopulations,
               ghostLevels,
               ghostLevelCount,
               arena,
               identity,
               definitions,
               peers,
               peerCount,
               moduleOwners,
               moduleOwnerCount,
               outbox,
               variables,
               timers,
               outboxRead,
               variableCount,
               timerCount,
               nextTimerSequence,
               nextIntentKey,
               stateRevision,
               callbacks,
               committedCallbacks,
               refusedCallbacks,
               collections,
               phase,
               initialStateRegion,
               initialStateSpawnSet,
               initialStateOmissions,
               initialStateOmissionCount,
               arenaBytesAfterClose,
               lastError,
               state,
               frame,
               startReference,
               eventReferences,
               loadReference,
               hasInitialState,
               active,
               faulted] = value;
        return std::tie(attempt,
                        deviceRequests,
                        deviceRequestSources,
                        squadPopulations,
                        ghostLevels,
                        ghostLevelCount,
                        arena,
                        identity,
                        definitions,
                        peers,
                        peerCount,
                        moduleOwners,
                        moduleOwnerCount,
                        outbox,
                        variables,
                        timers,
                        outboxRead,
                        variableCount,
                        timerCount,
                        nextTimerSequence,
                        nextIntentKey,
                        stateRevision,
                        callbacks,
                        committedCallbacks,
                        refusedCallbacks,
                        collections,
                        phase,
                        initialStateRegion,
                        initialStateSpawnSet,
                        initialStateOmissions,
                        initialStateOmissionCount,
                        arenaBytesAfterClose,
                        lastError,
                        state,
                        frame,
                        startReference,
                        eventReferences,
                        loadReference,
                        hasInitialState,
                        active,
                        faulted);
    };
    auto leftMembers = members(left);
    auto rightMembers = members(right);
    static_assert(noexcept(leftMembers.swap(rightMembers)));
    leftMembers.swap(rightMembers);
    for (Impl* impl : {&left, &right}) {
        if (impl->state != nullptr) {
            lua_setallocf(impl->state, &detail::arena_allocate, &impl->arena);
            detail::attach_impl(impl->state, *impl);
        }
    }
    return true;
}

/** Copies the attempt and the observations State does not hold: ghost levels and peers. */
bool copy_inputs(const Vm& source, Vm& prepared) noexcept {
    const Impl& from = VmAccess::get(source);
    Impl& to = VmAccess::get(prepared);
    if (from.frame != nullptr || to.frame != nullptr) {
        return false;
    }
    to.attempt = from.attempt;
    to.ghostLevels = from.ghostLevels;
    to.ghostLevelCount = from.ghostLevelCount;
    to.peers = from.peers;
    to.peerCount = from.peerCount;
    return true;
}

/** Closed VMs and unowned modules cannot match a mod-scoped reload. */
bool uses_mod(const Vm& vm, std::string_view modId) noexcept {
    const Impl& impl = VmAccess::get(vm);
    if (modId.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < impl.moduleOwnerCount; ++index) {
        if (std::string_view(impl.moduleOwners[index].data()) == modId) {
            return true;
        }
    }
    return false;
}

} // namespace sunrise::server::activity::mission::lua_vm
