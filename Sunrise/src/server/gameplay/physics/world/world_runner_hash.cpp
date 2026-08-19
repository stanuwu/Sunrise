#include <bit>
#include <type_traits>

#include "world_runner.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

/** FNV-1a offset basis gives an architecture-independent initial hash. */
constexpr std::uint64_t kHashOffsetBasis = 14'695'981'039'346'656'037ULL;
/** FNV-1a prime advances one canonical input byte. */
constexpr std::uint64_t kHashPrime = 1'099'511'628'211ULL;

/**
 * Adds one fixed integral value in little-endian byte order.
 * @tparam Value Integral or enum input type.
 * @param hash Running canonical hash.
 * @param value Value to append.
 */
template <typename Value> void hash_scalar(std::uint64_t& hash, Value value) noexcept {
    static_assert(std::is_integral_v<Value> || std::is_enum_v<Value>);
    if constexpr (std::is_enum_v<Value>) { // Enums hash through their declared base type.
        using Raw = std::underlying_type_t<Value>;
        using Unsigned = std::make_unsigned_t<Raw>;
        Unsigned raw = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(raw); ++index) {
            hash ^= static_cast<std::uint8_t>(raw & 0xFFU);
            hash *= kHashPrime;
            raw >>= 8U;
        }
    } else {
        using Unsigned = std::make_unsigned_t<Value>;
        Unsigned raw = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(raw); ++index) {
            hash ^= static_cast<std::uint8_t>(raw & 0xFFU);
            hash *= kHashPrime;
            raw >>= 8U;
        }
    }
}

/** @param hash Running hash. @param value Canonical IEEE-754 bits to append. */
void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_scalar(hash, std::bit_cast<std::uint32_t>(value));
}

/** @param hash Running hash. @param transform Canonical transform fields to append. */
void hash_transform(std::uint64_t& hash, const Transform& transform) noexcept {
    hash_float(hash, transform.position.x);
    hash_float(hash, transform.position.y);
    hash_float(hash, transform.position.z);
    hash_float(hash, transform.rotation.x);
    hash_float(hash, transform.rotation.y);
    hash_float(hash, transform.rotation.z);
    hash_float(hash, transform.rotation.w);
}

/** @param hash Running hash. @param header Exact retained command identity. */
void hash_command(std::uint64_t& hash, const HostCommandHeader& header) noexcept {
    hash_scalar(hash, header.world.activitySessionId);
    hash_scalar(hash, header.world.worldGeneration);
    hash_scalar(hash, header.world.ownerGeneration);
    hash_scalar(hash, header.definition.id);
    hash_scalar(hash, header.definition.version);
    hash_scalar(hash, header.source.id);
    hash_scalar(hash, header.source.kind);
    hash_scalar(hash, header.commandId);
    hash_scalar(hash, header.executeTick);
    hash_scalar(hash, header.policyOwnerId);
    hash_scalar(hash, header.sourceSequence);
}

} // namespace

/** @return Canonical hash of exact world identity, history, tick, and sorted actors. */
std::uint64_t compute_snapshot_hash(const WorldSnapshot& snapshot) noexcept {
    if (snapshot.actorCount > snapshot.actors.size()
        || snapshot.retiredActorCount > snapshot.retiredActors.size()
        || snapshot.commandHistoryCount > snapshot.commandHistory.size()) {
        return 0;
    }
    std::uint64_t hash = kHashOffsetBasis;
    hash_scalar(hash, snapshot.identity.activitySessionId);
    hash_scalar(hash, snapshot.identity.worldGeneration);
    hash_scalar(hash, snapshot.identity.ownerGeneration);
    hash_scalar(hash, snapshot.tick);
    hash_scalar(hash, snapshot.actorCount);
    for (std::size_t index = 0; index < snapshot.actorCount; ++index) {
        const ActorSnapshot& actor = snapshot.actors[index];
        hash_scalar(hash, actor.key.id);
        hash_scalar(hash, actor.key.generation);
        hash_scalar(hash, actor.policyOwnerId);
        hash_scalar(hash, actor.blueprint.id);
        hash_scalar(hash, actor.blueprint.version);
        hash_scalar(hash, actor.parent.id);
        hash_scalar(hash, actor.parent.generation);
        hash_transform(hash, actor.transform);
        hash_scalar(hash, actor.authority.ownerMemberId);
        hash_scalar(hash, actor.authority.generation);
        hash_scalar(hash, actor.authority.validFromTick);
        hash_scalar(hash, actor.authority.mode);
        hash_scalar(hash, actor.revision);
        hash_scalar(hash, actor.bubble);
        hash_scalar(hash, actor.lifecycle);
    }
    hash_scalar(hash, snapshot.retiredActorCount);
    for (std::size_t index = 0; index < snapshot.retiredActorCount; ++index) {
        const RetiredActorGeneration& retired = snapshot.retiredActors[index];
        hash_scalar(hash, retired.actorId);
        hash_scalar(hash, retired.generation);
        hash_scalar(hash, retired.retiredTick);
    }
    hash_scalar(hash, snapshot.commandHistoryCount);
    hash_scalar(hash, snapshot.commandHistoryCursor);
    for (std::size_t index = 0; index < snapshot.commandHistoryCount; ++index) {
        hash_command(hash, snapshot.commandHistory[index]);
    }
    return hash;
}

/** @return Canonical hash of the current committed world and dedupe window. */
std::uint64_t WorldRunner::compute_hash() const noexcept {
    WorldSnapshot snapshot{};
    snapshot.identity = identity_;
    snapshot.tick = tick_;
    snapshot.actorCount = actors_.copy_sorted(snapshot.actors);
    snapshot.retiredActorCount = actors_.copy_retired(snapshot.retiredActors);
    snapshot.commandHistory = commandHistory_;
    snapshot.commandHistoryCount = commandHistoryCount_;
    snapshot.commandHistoryCursor = commandHistoryCursor_;
    return compute_snapshot_hash(snapshot);
}

} // namespace sunrise::server::gameplay::physics::world
