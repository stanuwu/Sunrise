#include "proxy_profile_publisher.h"

#include <algorithm>
#include <array>

#include "../../../core/settings/settings.h"
#include "../../../middleware/bap/frame.h"
#include "../../../middleware/profile/public_profile_codec.h"
#include "../../../state/account/account_context.h"
#include "../../../state/account/public_profiles.h"
#include "../../../state/activity/fireteam.h"
#include "../../../state/activity/reservations/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../../state/social/steam_roster.h"
#include "proxy_internal.h"
#include "proxy_runtime.h"

namespace sunrise::server::bap::proxy::profile_publisher {
namespace {
constexpr std::uint64_t kPublishIntervalMs = 1'000;
struct Publication {
    std::uint64_t attemptedGeneration{};
    std::uint64_t ackedGeneration{};
    std::uint32_t outstandingTaskId{};
    std::uint32_t connectionId{};
    std::uint64_t nextAttemptTick{};
    std::size_t attemptedSize{};
    std::size_t ackedSize{};
    bool acknowledged{};
};
Publication g_publication;
std::array<std::byte, middleware::profile::kMaximumEncodedSize> g_composed{};
std::array<std::byte, middleware::profile::kMaximumEncodedSize> g_acknowledged{};
// The BAP session lock serializes this producer with incoming profile/directory commits.
state::AccountState g_snapshot;
state::AccountState g_decodeScratch;
state::social::Hub g_directory;
} // namespace

void reset() noexcept {
    g_publication = {};
}

bool acknowledge(std::uint32_t connectionId,
                 std::uint32_t taskId,
                 std::uint16_t responseService) noexcept {
    if (connectionId != g_publication.connectionId || g_publication.outstandingTaskId == 0
        || taskId != g_publication.outstandingTaskId
        || responseService
               != static_cast<std::uint16_t>(middleware::bap::ResponseService::accountProjection)) {
        return false;
    }
    g_publication.outstandingTaskId = 0;
    g_publication.ackedGeneration = g_publication.attemptedGeneration;
    g_publication.ackedSize = g_publication.attemptedSize;
    g_publication.acknowledged = true;
    std::copy_n(g_composed.begin(), g_publication.ackedSize, g_acknowledged.begin());
    report(0, "project", "acked", responseService, taskId, "-");
    return true;
}

void abandon(std::uint32_t connectionId,
             std::uint32_t taskId,
             std::uint16_t responseService) noexcept {
    if (connectionId == g_publication.connectionId && g_publication.outstandingTaskId != 0
        && taskId == g_publication.outstandingTaskId) {
        g_publication.outstandingTaskId = 0;
        report(0, "project", "fail", responseService, taskId, "abandoned");
    }
}

void service(std::uint64_t now) noexcept {
    const bool localHost = core::settings::role() == core::settings::Role::host;
    if ((!localHost && !core::settings::get().server.upstream.enabled)
        || g_publication.outstandingTaskId != 0 || now < g_publication.nextAttemptTick) {
        return;
    }
    const auto generation = state::account::profiles::local_generation();
    if (g_publication.acknowledged && generation == g_publication.ackedGeneration) {
        return;
    }
    const auto connectionId = first_ready_upstream();
    if (!localHost && connectionId == 0) {
        return;
    }
    g_publication.nextAttemptTick = now + kPublishIntervalMs;
    const state::ScopedAccount local(state::kLocalAccount);
    std::size_t size = 0;
    if (!state::local_account_snapshot(g_snapshot)) {
        return;
    }
    g_snapshot.presence.artifactPowerBonus = state::artifact_power_bonus();
    if (!middleware::profile::encode(g_snapshot, g_composed, size)) {
        return;
    }
    // Private-only writes and repeated canonicalization do not publish identical public records.
    if (g_publication.acknowledged && size == g_publication.ackedSize
        && std::equal(g_composed.begin(),
                      g_composed.begin() + static_cast<std::ptrdiff_t>(size),
                      g_acknowledged.begin())) {
        g_publication.ackedGeneration = generation;
        return;
    }
    if (localHost) {
        namespace social = state::social;
        namespace profiles = state::account::profiles;
        // The outgoing bytes are complete; reuse their source image to read the prior cache.
        const bool hadPrevious = profiles::snapshot(state::kLocalAccount, g_snapshot);
        const auto previousNative = g_snapshot.presence.native;
        // Use the same public wire projection as joining players. SQLite remains the owner.
        if (!middleware::profile::decode(
                std::span(g_composed).first(size), g_snapshot, g_decodeScratch)) {
            return;
        }
        g_directory = social::session_directory();
        social::RosterEntry row{};
        row.primarySoid = g_snapshot.primarySoid;
        row.steamId = g_snapshot.presence.platformId;
        row.personaName = g_snapshot.presence.personaName;
        const auto membership = profiles::membership_generation(state::kLocalAccount);
        if (!g_directory.publish(state::kLocalAccount, row)
            || !profiles::publish(state::kLocalAccount, g_snapshot)) {
            return;
        }
        social::session_directory() = g_directory;
        if (hadPrevious
            && state::activity::fireteam::native_solo_split(previousNative,
                                                            g_snapshot.presence.native)) {
            static_cast<void>(state::activity::fireteam::depart(row.primarySoid));
        }
        if (membership != profiles::membership_generation(state::kLocalAccount)) {
            state::activity::reservations::invalidate_owner(row.primarySoid);
        }
        g_publication.ackedGeneration = generation;
        g_publication.ackedSize = size;
        g_publication.acknowledged = true;
        std::copy_n(g_composed.begin(), size, g_acknowledged.begin());
        return;
    }
    std::uint32_t taskId = 0;
    if (!send_upstream_request(
            connectionId,
            static_cast<std::uint16_t>(middleware::bap::RequestService::accountProjection),
            static_cast<std::uint16_t>(middleware::bap::ResponseService::accountProjection),
            std::span(g_composed).first(size),
            taskId)) {
        return;
    }
    g_publication.outstandingTaskId = taskId;
    g_publication.connectionId = connectionId;
    g_publication.attemptedGeneration = generation;
    g_publication.attemptedSize = size;
    report(connectionId,
           "project",
           "sent",
           static_cast<std::uint16_t>(middleware::bap::RequestService::accountProjection),
           taskId,
           "-");
}

} // namespace sunrise::server::bap::proxy::profile_publisher
