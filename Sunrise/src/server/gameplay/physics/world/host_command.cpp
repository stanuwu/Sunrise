#include "host_command.h"

#include <limits>

namespace sunrise::server::gameplay::physics::world {

HostCommandKind command_kind(const HostCommand& command) noexcept {
    const std::size_t index = command.payload.index();
    if (index >= static_cast<std::size_t>(HostCommandKind::count)) {
        return HostCommandKind::count;
    }
    return static_cast<HostCommandKind>(index);
}

/**
 * Classifies commands guarded by direct-host actor permission.
 * @param kind Stable closed command kind.
 * @return True when the payload reads or mutates actor state.
 */
bool command_uses_actor(HostCommandKind kind) noexcept {
    switch (kind) {
    case HostCommandKind::spawnActor:
    case HostCommandKind::removeActor:
    case HostCommandKind::setMotionAuthority:
    case HostCommandKind::setKinematicTarget:
    case HostCommandKind::teleportActor:
    case HostCommandKind::configureController:
    case HostCommandKind::requestPath:
    case HostCommandKind::configureCombat:
    case HostCommandKind::submitDamage:
    case HostCommandKind::emitMappedIncident:
        return true;
    case HostCommandKind::createTrigger:
    case HostCommandKind::removeTrigger:
    case HostCommandKind::addObjectiveCounter:
    case HostCommandKind::setObjectiveCounter:
    case HostCommandKind::recordParticipantCredit:
    case HostCommandKind::scheduleTickTimer:
    case HostCommandKind::requestCheckpoint:
    case HostCommandKind::submitRewardIntent:
    case HostCommandKind::count:
        return false;
    }
    return false;
}

std::uint64_t command_kind_mask(HostCommandKind kind) noexcept {
    const auto index = static_cast<std::uint8_t>(kind);
    if (kind == HostCommandKind::count || index >= std::numeric_limits<std::uint64_t>::digits) {
        return 0;
    }
    return std::uint64_t{1} << index;
}

bool valid_command_header(const HostCommandHeader& header) noexcept {
    return valid_world(header.world) && valid_blueprint(header.definition) && header.source.id != 0
           && header.commandId != 0 && header.executeTick != 0 && header.policyOwnerId != 0
           && header.sourceSequence != 0
           && static_cast<std::uint8_t>(header.source.kind)
                  < static_cast<std::uint8_t>(CommandSourceKind::count);
}

} // namespace sunrise::server::gameplay::physics::world
