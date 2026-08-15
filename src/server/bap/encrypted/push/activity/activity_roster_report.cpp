#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

/** Log names for each outcome, in the enum's own order. */
constexpr std::array<const char*, 5> kOutcomeNames = {
    "ok", "no_epoch", "no_layout", "no_groups", "encode"};

} // namespace

/** Reports one roster push, and only when its outcome is new. */
void report_roster_push(Session& session,
                        const message::Snapshot& snapshot,
                        std::string_view destination,
                        std::size_t bytes,
                        std::int32_t grant,
                        RosterOutcome outcome) noexcept {
    const message::Roster& roster = snapshot.roster;
    const auto reason = static_cast<std::uint8_t>(outcome) + 1U;
    // A published push reports every time. A refusal reports only when its reason is new.
    if (outcome != RosterOutcome::published && session.activityRosterReason == reason) {
        return;
    }
    session.activityRosterReason = static_cast<std::uint8_t>(reason);
    std::size_t slots = 0;
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        slots += roster.groups[index].slotTypes.size();
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=roster result=%s dest=%.*s groups=%zu "
                      "objects=%zu bytes=%zu state=%u keygroup=0x%X grant=%d "
                      "region=%u slice=%u spawn=0x%X join=0x%llX player=0x%llX",
                      kOutcomeNames[static_cast<std::size_t>(outcome)],
                      static_cast<int>(destination.size()),
                      destination.data(),
                      roster.groupCount,
                      slots,
                      bytes,
                      session.activityRosterState,
                      roster.playerKeyGroup,
                      grant,
                      snapshot.region,
                      snapshot.spawnSliceSet,
                      snapshot.spawnSetHash,
                      static_cast<unsigned long long>(session.activityCharacterSoid),
                      static_cast<unsigned long long>(snapshot.playerKey));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         outcome == RosterOutcome::published ? core::log::Level::debug
                                                             : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::server::bap::encrypted::push::activity
