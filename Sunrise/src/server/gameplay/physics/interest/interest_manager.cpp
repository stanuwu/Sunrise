#include "interest_manager.h"

#include <algorithm>
#include <cmath>

namespace sunrise::server::gameplay::physics::interest {
namespace {

[[nodiscard]] bool equal_peer(const PeerKey& left, const PeerKey& right) noexcept {
    return left.memberId == right.memberId && left.viewGeneration == right.viewGeneration
           && left.activitySessionId == right.activitySessionId;
}

[[nodiscard]] bool valid_peer(const PeerKey& key) noexcept {
    return key.memberId != 0 && key.viewGeneration != 0 && key.activitySessionId != 0;
}

[[nodiscard]] bool finite_view(const PeerView& peer) noexcept {
    return world::finite_transform({peer.position, {}}) && std::isfinite(peer.enterDistance)
           && std::isfinite(peer.leaveDistance) && peer.enterDistance >= 0.0F
           && peer.leaveDistance >= peer.enterDistance;
}

[[nodiscard]] double distance_squared(const world::Vector3& left,
                                      const world::Vector3& right) noexcept {
    const double x = static_cast<double>(left.x) - static_cast<double>(right.x);
    const double y = static_cast<double>(left.y) - static_cast<double>(right.y);
    const double z = static_cast<double>(left.z) - static_cast<double>(right.z);
    return x * x + y * y + z * z;
}

/** @return True when every actor key is valid and appears exactly once. */
[[nodiscard]] bool unique_actors(std::span<const ActorInput> actors) noexcept {
    for (std::size_t index = 0; index < actors.size(); ++index) {
        if (!world::valid_actor(actors[index].key)
            || !world::finite_transform({actors[index].position, {}})) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (world::equal_actor(actors[prior].key, actors[index].key)) {
                return false;
            }
        }
    }
    return true;
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

[[nodiscard]] bool valid_dependency(std::span<const ActorInput> actors,
                                    const ActorInput& owner,
                                    const world::ActorKey& dependency) noexcept {
    if (!world::valid_actor(dependency)) {
        return true;
    }
    const ActorInput* resolved = find_actor(actors, dependency);
    return !world::equal_actor(owner.key, dependency) && resolved != nullptr
           && resolved->bubble == owner.bubble;
}

} // namespace

/** Clears every peer and relevance baseline. */
void InterestManager::reset() noexcept {
    peers_ = {};
    peerCount_ = 0;
}

/** Binds a fresh exact peer/view lifetime. */
bool InterestManager::bind_peer(const PeerKey& key) noexcept {
    if (!valid_peer(key)) {
        return false;
    }
    if (find_peer(key) != nullptr) {
        return true;
    }
    for (PeerRecord& peer : peers_) {
        if (!peer.occupied) {
            peer = {};
            peer.key = key;
            peer.occupied = true;
            ++peerCount_;
            return true;
        }
    }
    return false;
}

/** Removes one exact peer/view lifetime. */
bool InterestManager::unbind_peer(const PeerKey& key) noexcept {
    PeerRecord* peer = find_peer(key);
    if (peer == nullptr) {
        return false;
    }
    *peer = {};
    --peerCount_;
    return true;
}

/** Applies activity, bubble, hysteresis, and parent closure in one commit. */
bool InterestManager::evaluate(const world::WorldIdentity& identity,
                               const PeerView& view,
                               std::span<const ActorInput> actors,
                               Evaluation& output) noexcept {
    output = {};
    PeerRecord* record = find_peer(view.key);
    if (record == nullptr || !world::valid_world(identity)
        || view.key.activitySessionId != identity.activitySessionId || !finite_view(view)
        || actors.size() > world::kActorCapacity || !unique_actors(actors)) {
        return false;
    }
    PeerRecord baseline = *record;
    if (baseline.hasWorld && !world::equal_world(baseline.world, identity)) {
        baseline.actors = {};
    }

    std::array<world::ActorKey, world::kActorCapacity> next{};
    std::size_t nextCount = 0;
    for (const ActorInput& actor : actors) {
        if (!valid_dependency(actors, actor, actor.parent)
            || !valid_dependency(actors, actor, actor.streamSource)) {
            return false;
        }
        const bool previous = was_relevant(baseline, actor.key);
        const double limit = previous ? view.leaveDistance : view.enterDistance;
        const bool spatial = distance_squared(actor.position, view.position) <= limit * limit;
        if (actor.bubble == view.bubble && (actor.alwaysRelevant || spatial)) {
            next[nextCount++] = actor.key;
        }
    }

    for (std::size_t pass = 0; pass < actors.size(); ++pass) {
        bool changed = false;
        for (std::size_t index = 0; index < nextCount; ++index) {
            const ActorInput* actor = find_actor(actors, next[index]);
            if (actor != nullptr && world::valid_actor(actor->parent)
                && !contains({next.data(), nextCount}, actor->parent)) {
                const ActorInput* parent = find_actor(actors, actor->parent);
                if (parent == nullptr || nextCount == next.size()) {
                    return false;
                }
                next[nextCount++] = parent->key;
                changed = true;
            }
            if (actor != nullptr && world::valid_actor(actor->streamSource)
                && !contains({next.data(), nextCount}, actor->streamSource)) {
                const ActorInput* source = find_actor(actors, actor->streamSource);
                if (source == nullptr || nextCount == next.size()) {
                    return false;
                }
                next[nextCount++] = source->key;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    if (!order_transitions(baseline, actors, {next.data(), nextCount}, output)) {
        return false;
    }

    record->actors = {};
    for (std::size_t index = 0; index < nextCount; ++index) {
        const ActorInput* actor = find_actor(actors, next[index]);
        if (actor == nullptr) {
            return false;
        }
        record->actors[index] = {next[index], actor->parent, actor->streamSource, true};
    }
    record->world = identity;
    record->hasWorld = true;
    output.count = std::min(output.count, output.transitions.size());
    return true;
}

/** Returns the number of exact peer/view lifetimes. */
std::size_t InterestManager::peer_count() const noexcept {
    return peerCount_;
}

InterestManager::PeerRecord* InterestManager::find_peer(const PeerKey& key) noexcept {
    for (PeerRecord& peer : peers_) {
        if (peer.occupied && equal_peer(peer.key, key)) {
            return &peer;
        }
    }
    return nullptr;
}

const InterestManager::PeerRecord* InterestManager::find_peer(const PeerKey& key) const noexcept {
    for (const PeerRecord& peer : peers_) {
        if (peer.occupied && equal_peer(peer.key, key)) {
            return &peer;
        }
    }
    return nullptr;
}

bool InterestManager::was_relevant(const PeerRecord& peer, const world::ActorKey& key) noexcept {
    for (const RelevantActor& actor : peer.actors) {
        if (actor.occupied && world::equal_actor(actor.key, key)) {
            return true;
        }
    }
    return false;
}

} // namespace sunrise::server::gameplay::physics::interest
