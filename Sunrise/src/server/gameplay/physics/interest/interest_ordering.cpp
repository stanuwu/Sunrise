#include <algorithm>

#include "interest_manager.h"

namespace sunrise::server::gameplay::physics::interest {
namespace {

[[nodiscard]] bool actor_less(const world::ActorKey& left, const world::ActorKey& right) noexcept {
    return left.id < right.id || (left.id == right.id && left.generation < right.generation);
}

[[nodiscard]] bool contains(std::span<const world::ActorKey> actors,
                            const world::ActorKey& key) noexcept {
    return std::find_if(
               actors.begin(),
               actors.end(),
               [&](const world::ActorKey& candidate) { return world::equal_actor(candidate, key); })
           != actors.end();
}

[[nodiscard]] const ActorInput* find_actor(std::span<const ActorInput> actors,
                                           const world::ActorKey& key) noexcept {
    const auto found = std::find_if(actors.begin(), actors.end(), [&](const ActorInput& actor) {
        return world::equal_actor(actor.key, key);
    });
    return found == actors.end() ? nullptr : &*found;
}

[[nodiscard]] bool depends_on(const world::ActorKey& parent,
                              const world::ActorKey& source,
                              const world::ActorKey& dependency) noexcept {
    return (world::valid_actor(parent) && world::equal_actor(parent, dependency))
           || (world::valid_actor(source) && world::equal_actor(source, dependency));
}

} // namespace

/** Orders departed actors after dependents and current actors after dependencies. */
bool InterestManager::order_transitions(const PeerRecord& peer,
                                        std::span<const ActorInput> actors,
                                        std::span<const world::ActorKey> next,
                                        Evaluation& output) noexcept {
    std::array<bool, world::kActorCapacity> emitted{};
    std::size_t remaining = 0;
    for (std::size_t index = 0; index < peer.actors.size(); ++index) {
        remaining +=
            peer.actors[index].occupied && !contains(next, peer.actors[index].key) ? 1U : 0U;
    }
    while (remaining != 0) {
        std::size_t selected = peer.actors.size();
        for (std::size_t index = 0; index < peer.actors.size(); ++index) {
            const RelevantActor& candidate = peer.actors[index];
            if (!candidate.occupied || emitted[index] || contains(next, candidate.key)) {
                continue;
            }
            bool hasDependent = false;
            for (std::size_t other = 0; other < peer.actors.size(); ++other) {
                const RelevantActor& dependent = peer.actors[other];
                if (dependent.occupied && !emitted[other] && !contains(next, dependent.key)
                    && depends_on(dependent.parent, dependent.streamSource, candidate.key)) {
                    hasDependent = true;
                    break;
                }
            }
            if (!hasDependent
                && (selected == peer.actors.size()
                    || actor_less(candidate.key, peer.actors[selected].key))) {
                selected = index;
            }
        }
        if (selected == peer.actors.size()) {
            return false;
        }
        output.transitions[output.count++] = {peer.actors[selected].key, TransitionKind::left};
        emitted[selected] = true;
        --remaining;
    }

    emitted = {};
    remaining = next.size();
    while (remaining != 0) {
        std::size_t selected = next.size();
        for (std::size_t index = 0; index < next.size(); ++index) {
            if (emitted[index]) {
                continue;
            }
            const ActorInput* candidate = find_actor(actors, next[index]);
            if (candidate == nullptr) {
                return false;
            }
            const auto ready = [&](const world::ActorKey& dependency) {
                if (!world::valid_actor(dependency) || !contains(next, dependency)) {
                    return true;
                }
                for (std::size_t prior = 0; prior < next.size(); ++prior) {
                    if (emitted[prior] && world::equal_actor(next[prior], dependency)) {
                        return true;
                    }
                }
                return false;
            };
            if (ready(candidate->parent) && ready(candidate->streamSource)
                && (selected == next.size() || actor_less(next[index], next[selected]))) {
                selected = index;
            }
        }
        if (selected == next.size()) {
            return false;
        }
        const TransitionKind kind =
            was_relevant(peer, next[selected]) ? TransitionKind::stayed : TransitionKind::entered;
        output.transitions[output.count++] = {next[selected], kind};
        emitted[selected] = true;
        --remaining;
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::interest
