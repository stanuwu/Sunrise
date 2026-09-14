#include "account_projection_route.h"

#include <algorithm>
#include <memory>
#include <new>

#include "../../../core/settings/settings.h"
#include "../../../middleware/profile/public_profile_codec.h"
#include "../../../middleware/secure_channel/runtime.h"
#include "../../../state/account/public_profiles.h"
#include "../../../state/activity/fireteam.h"
#include "../../../state/activity/reservations/runtime.h"
#include "../../../state/social/steam_roster.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted {
bool consume_account_projection(Session& session,
                                Scratch& scratch,
                                const middleware::bap::RequestFrame& request,
                                std::span<std::byte> response,
                                std::size_t& written) noexcept {
    written = 0;
    if (!core::settings::hosts_session() || !session.authenticated
        || request.frameType != middleware::bap::FrameType::encrypted
        || request.serviceId
               != static_cast<std::uint16_t>(middleware::bap::RequestService::accountProjection)
        || session.accountHandle == state::kLocalAccount
        || session.accountHandle == state::kInvalidAccount) {
        return false;
    }
    auto profile = std::unique_ptr<state::AccountState>(new (std::nothrow) state::AccountState{});
    if (!profile || !middleware::profile::decode(request.body, *profile)) {
        return false;
    }
    auto directory = std::unique_ptr<state::social::Hub>(
        new (std::nothrow) state::social::Hub(state::social::session_directory()));
    state::social::RosterEntry row{};
    row.primarySoid = profile->primarySoid;
    row.steamId = profile->presence.platformId;
    row.personaName = profile->presence.personaName[0] != '\0' ? profile->presence.personaName
                                                               : profile->presence.displayName;
    if (!directory || !directory->publish(session.accountHandle, row)) {
        return false;
    }
    const ServiceRoute route{
        ResponseMode::reply, middleware::bap::ResponseService::accountProjection, BodyCodec::empty};
    std::size_t framedSize{};
    const auto membershipGeneration =
        state::account::profiles::membership_generation(session.accountHandle);
    const auto previousNative = state::account::profiles::native_presence(session.accountHandle);
    if (!reply::encode(
            scratch, route, request.taskId, session.sessionKey, session.sendNonce, {}, framedSize)
        || framedSize > response.size()
        || !state::account::profiles::publish(session.accountHandle, *profile)) {
        return false;
    }
    state::social::session_directory() = *directory;
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    middleware::secure_channel::advance_nonce(session.sendNonce);
    const auto& native = profile->presence.native;
    if (native.characterSoid == state::account::profiles::selected_character(session.accountHandle)
        && state::activity::fireteam::native_solo_split(previousNative, native)) {
        static_cast<void>(state::activity::fireteam::depart(profile->primarySoid));
    }
    if (membershipGeneration
        != state::account::profiles::membership_generation(session.accountHandle)) {
        state::activity::reservations::invalidate_owner(profile->primarySoid);
    }
    written = framedSize;
    return true;
}
} // namespace sunrise::server::bap::encrypted
