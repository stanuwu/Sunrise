#include "group_migration_receipts.h"

#include "../../../middleware/gameplay/group/migration_messages.h"
#include "../../../middleware/gameplay/group/notice_messages.h"
#include "../gameplay_log.h"

namespace sunrise::server::gameplay::group::migration {

namespace {

namespace wire = middleware::gameplay::group;
namespace bits = middleware::encoding::bits;

/** @return True when the id names a migration body carrying nothing but a group session. */
[[nodiscard]] bool session_only(std::uint8_t id) noexcept {
    return id == static_cast<std::uint8_t>(wire::MigrationMessageId::reestablishPending)
           || id == static_cast<std::uint8_t>(wire::MigrationMessageId::peerReestablish);
}

/**
 * Reads a handoff or its acknowledgement.
 * @param id Registry message id, which names which half of the pair this is.
 * @param reader Reader positioned at the body.
 * @return True when the body was completely read.
 */
[[nodiscard]] bool consume_handoff(std::uint8_t id, bits::Reader& reader) noexcept {
    wire::HostHandoff body{};
    if (!wire::read_host_handoff(reader, body)) {
        return false;
    }
    report(core::log::Level::info,
           "ev=gameplay stage=migration result=handoff id=%u session=0x%016llX successor=%u",
           static_cast<unsigned>(id),
           static_cast<unsigned long long>(body.sessionId),
           static_cast<unsigned>(body.successorIndex));
    return true;
}

/**
 * Reads one body a group host normally emits and records what it said.
 * @param id Registry message id.
 * @param reader Reader positioned at the body.
 * @return True when the id is one of these messages and its body was completely read.
 */
[[nodiscard]] bool consume_notice(std::uint8_t id, bits::Reader& reader) noexcept {
    if (id == static_cast<std::uint8_t>(wire::NoticeMessageId::peerConnectNotice)) {
        wire::PeerConnectNotice body{};
        if (!wire::read_peer_connect(reader, body)) {
            return false;
        }
        report(core::log::Level::debug,
               "ev=gameplay stage=notice result=peer_connect session=0x%016llX machine=0x%016llX "
               "protocol=0x%04X",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned long long>(body.machineId),
               static_cast<unsigned>(body.protocolVersion));
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::NoticeMessageId::sessionBootNotice)) {
        wire::SessionBootNotice body{};
        if (!wire::read_session_boot(reader, body)) {
            return false;
        }
        report(core::log::Level::warn,
               "ev=gameplay stage=notice result=boot session=0x%016llX kind=%u reason=%u",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned>(body.kind),
               static_cast<unsigned>(body.reason));
        return true;
    }
    const bool delegate =
        id == static_cast<std::uint8_t>(wire::NoticeMessageId::delegateLeadership);
    if (delegate || id == static_cast<std::uint8_t>(wire::NoticeMessageId::bootMachine)) {
        wire::AddressedNotice body{};
        if (!wire::read_addressed_notice(reader, !delegate, body)) {
            return false;
        }
        report(core::log::Level::warn,
               "ev=gameplay stage=notice result=%s session=0x%016llX kind=%u",
               delegate ? "delegate" : "boot_machine",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned>(body.kind));
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::NoticeMessageId::playerRefuse)) {
        wire::PlayerRefuse body{};
        if (!wire::read_player_refuse(reader, body)) {
            return false;
        }
        report(core::log::Level::warn,
               "ev=gameplay stage=notice result=player_refuse session=0x%016llX player=0x%016llX "
               "reason=%u",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned long long>(body.playerId),
               static_cast<unsigned>(body.reason));
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::NoticeMessageId::voiceRegistration)) {
        if (!wire::read_voice_registration(reader)) {
            return false;
        }
        report(core::log::Level::debug, "ev=gameplay stage=notice result=voice");
        return true;
    }
    return false;
}

} // namespace

/** Reads one host-migration or election message and records what it said. */
bool consume(std::uint8_t id, bits::Reader& reader) noexcept {
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::hostHandoff)
        || id == static_cast<std::uint8_t>(wire::MigrationMessageId::peerHandoff)) {
        return consume_handoff(id, reader);
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::hostTransition)) {
        wire::HostTransition body{};
        if (!wire::read_host_transition(reader, body)) {
            return false;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=migration result=transition session=0x%016llX progress=%u "
               "token=0x%08X",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned>(body.progress),
               body.transitionToken);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::hostReestablish)) {
        wire::HostReestablish body{};
        if (!wire::read_host_reestablish(reader, body)) {
            return false;
        }
        // The new host is recorded and not installed. Installing one from a peer's own claim is
        // how a group ends up with two hosts.
        report(core::log::Level::warn,
               "ev=gameplay stage=migration result=reestablish session=0x%016llX machine=0x%016llX",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned long long>(body.machineId));
        return true;
    }
    if (session_only(id)) {
        std::uint64_t sessionId = 0;
        if (!wire::read_migration_session(reader, sessionId)) {
            return false;
        }
        report(core::log::Level::debug,
               "ev=gameplay stage=migration result=pending id=%u session=0x%016llX",
               static_cast<unsigned>(id),
               static_cast<unsigned long long>(sessionId));
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::hostDecline)) {
        wire::HostDecline body{};
        if (!wire::read_host_decline(reader, body)) {
            return false;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=migration result=decline session=0x%016llX data=%u flag=%u",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned>(body.hasDeclineData),
               static_cast<unsigned>(body.declineFlag));
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::election)) {
        wire::Election body{};
        if (!wire::read_election(reader, body)) {
            return false;
        }
        report(core::log::Level::warn,
               "ev=gameplay stage=migration result=election session=0x%016llX candidates=%u "
               "tail=%u",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned>(body.candidateCount),
               body.tailBits);
        // The candidate value width is unrecovered, so the bitsets behind the addresses were not
        // located and no later message in this container can be found.
        return false;
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::electionRefuse)) {
        wire::ElectionRefuse body{};
        if (!wire::read_election_refuse(reader, body)) {
            return false;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=migration result=election_refuse session=0x%016llX code=%u",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned>(body.refuseCode));
        return true;
    }
    return consume_notice(id, reader);
}

} // namespace sunrise::server::gameplay::group::migration
