#include "activity_host_manager_route.h"

#include <array>
#include <climits>
#include <cstdio>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/role.h"
#include "../../../../middleware/bap/activity_host_manager/request/activity_manager_request.h"
#include "../../../../middleware/bap/activity_host_manager/request/selection/\
activity_manager_selection_parser.h"
#include "../../../../middleware/bap/activity_host_manager/response/activity_manager_response.h"
#include "../../../../state/account/account_context.h"
#include "../../../../state/account/public_profiles.h"
#include "../../../../state/activity/defaults/activity_defaults_snapshot.h"
#include "../../../../state/activity/forced/activity_forced_destination.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/activity/shared_allocation.h"
#include "../../../../state/build_data/runtime.h"
#include "activity_establish_response.h"

namespace sunrise::server::bap::encrypted::activity_host_manager {
namespace {

namespace request_selection = middleware::bap::activity_host_manager::request::selection;

/** @param source Parsed copy. @return Its package name as text, empty when it carried none. */
[[nodiscard]] std::string_view
copy_name(const request_selection::ActivityManagerSelection& source) noexcept {
    return source.hasPackageName
               ? std::string_view(reinterpret_cast<const char*>(source.packageName.data()),
                                  source.packageNameLength)
               : std::string_view{};
}

/**
 * Picks between the request's two copies of the descriptor.
 * Field 1 wins by default. When the copies disagree the destination table breaks the tie: an
 * unknown name gives no bubble layout at all, so the copy naming a known destination wins.
 *
 * @param parsed Both copies with their presence.
 * @return The copy to build the session from.
 */
[[nodiscard]] const request_selection::ActivityManagerSelection&
choose_copy(const request_selection::ActivityManagerSelectionResult& parsed) noexcept {
    if (!parsed.hasSelection) {
        return parsed.secondary;
    }
    if (!parsed.hasSecondary) {
        return parsed.selection;
    }
    ::sunrise::state::build_data::scenarios::Definition layout{};
    const bool primaryKnown =
        ::sunrise::state::build_data::find_scenario_layout(copy_name(parsed.selection), layout);
    const bool secondaryKnown =
        ::sunrise::state::build_data::find_scenario_layout(copy_name(parsed.secondary), layout);
    return !primaryKnown && secondaryKnown ? parsed.secondary : parsed.selection;
}

/**
 * Reports the destination an operator forced, so a run shows where the client was sent.
 * @param forced Destination the forced selection produced.
 */
void report_forced(const state::activity::destination::DestinationSelection& forced) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bap svc=6 stage=selection result=forced name=%.*s "
                                      "bubble=%u slice_set=%u spawn=0x%X",
                                      static_cast<int>(forced.packageNameLength),
                                      reinterpret_cast<const char*>(forced.packageName.data()),
                                      static_cast<unsigned>(forced.arrivalBubbleOverride),
                                      static_cast<unsigned>(forced.sliceSetOverride),
                                      forced.spawnSetOverride);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Prepares an allocation from an optional typed field-1 destination.
 * The launch State does not need the parser-only request-kind, skull, and trailing fields.
 * @param parsed Scalar-only middleware result; holds no borrowed request bytes.
 * @param sessionId Cleared, then receives the chosen runtime activity id.
 * @param allocation Cleared, then receives the deferred State transaction.
 * @return True when the explicit selection or the State fallback can be prepared.
 */
[[nodiscard]] bool
prepare_allocation(const request_selection::ActivityManagerSelectionResult& parsed,
                   std::uint64_t& sessionId,
                   state::activity::PendingAllocation& allocation,
                   const state::activity::LaunchParty* party) noexcept {
    const bool hasCopy = parsed.hasSelection || parsed.hasSecondary;
    const request_selection::ActivityManagerSelection& source =
        hasCopy ? choose_copy(parsed) : parsed.selection;
    // An index-only selection cannot be mixed with unrelated authored package bytes, so it takes
    // the same State fallback an absent selection takes. Refusing would leave the request
    // unanswered, and svc 6 declares a response service.
    if (hasCopy && !source.hasPackageName) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=6 stage=package result=fallback");
    }
    // Neither case captures a descriptor, so a forced destination there sends the minimal one.
    // The State fallback still stands when the switch is off.
    if (!hasCopy || !source.hasPackageName) {
        state::activity::destination::DestinationSelection forced{};
        if (state::activity::forced::apply(forced)) {
            report_forced(forced);
            return party ? state::activity::prepare_shared_session(
                               forced, *party, sessionId, allocation)
                         : state::activity::prepare_session(forced, sessionId, allocation);
        }
        return party ? state::activity::prepare_shared_session(*party, sessionId, allocation)
                     : state::activity::prepare_session(sessionId, allocation);
    }
    state::activity::destination::DestinationSelection destination{};
    destination.packageName = source.packageName;
    destination.packageNameLength = source.packageNameLength;
    destination.reason = source.reason;
    destination.sourceActivityIndex = source.sourceActivityIndex;
    destination.activityIndex = source.activityIndex;
    destination.hasElementIndex = source.hasElementIndex;
    destination.elementIndex = source.hasElementIndex
                                   ? source.elementIndex
                                   : state::activity::destination::kAbsentElementIndex;
    destination.hasSelectionNonce = source.hasSelectionNonce;
    destination.selectionNonce = source.selectionNonce;
    destination.hasArrivalBubbleHash = source.hasArrivalBubbleHash;
    destination.arrivalBubbleHash = source.arrivalBubbleHash;
    destination.hasSpawnSetHash = source.hasSpawnSetHash;
    destination.spawnSetHash = source.spawnSetHash;
    // The descriptor is echoed back as sent, so its bits travel with the selection instead of being
    // rebuilt from the scalars above. A descriptor too wide for storage carries none, and the echo
    // falls back to the configured form.
    if (source.descriptorBitLength != 0
        && source.descriptorBitLength <= destination.descriptorBits.size() * CHAR_BIT) {
        destination.descriptorBits = source.descriptorBits;
        destination.descriptorBitLength = static_cast<std::uint16_t>(source.descriptorBitLength);
        destination.descriptorNameBit = static_cast<std::uint16_t>(source.packageNameBitOffset);
        destination.hasDescriptorName = source.hasPackageName;
    }
    // Forced lands last and renames the captured descriptor in place.
    if (state::activity::forced::apply(destination)) {
        report_forced(destination);
    }
    return party
               ? state::activity::prepare_shared_session(destination, *party, sessionId, allocation)
               : state::activity::prepare_session(destination, sessionId, allocation);
}

} // namespace

/** Prepares one runtime activity session and encodes its svc-7 response body. */
bool encode_response(std::span<const std::byte> requestBody,
                     std::span<std::byte> output,
                     std::size_t& written,
                     state::activity::PendingAllocation& allocation,
                     bool& hasAllocation,
                     PendingStartupReservations& startupReservations) noexcept {
    written = 0;
    startupReservations = {};
    allocation = {};
    hasAllocation = false;
    middleware::bap::activity_host_manager::Request request;
    request_selection::ActivityManagerSelectionResult selection{};
    // Reading the destination out of the PUT is best effort. A malformed PUT must never cost the
    // reply. The client rejects any service-7 body that is not exactly 137 bytes, so one
    // unsupported field would otherwise strand the activity route.
    if (!middleware::bap::activity_host_manager::request::parse_request(requestBody, request)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=6 stage=request result=fallback");
    } else if (!request_selection::parse_selection(request.protobuf, selection)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=6 stage=selection result=fallback");
        selection = {};
    }
    std::uint64_t sessionId = state::activity::kAbsentSessionId;
    state::activity::PendingAllocation prepared{};
    const auto owner = state::bound_account();
    const auto startup = prepare_startup_reservations(selection.startupReservations,
                                                      state::account_primary_soid(owner));
    state::activity::LaunchParty party{};
    const bool shared = core::settings::hosts_session();
    if (shared) {
        party.publisherAccount = state::account_primary_soid(owner);
        party.publisherCharacter = state::account::profiles::selected_character(owner);
        if (startup.valid) {
            party.identities = startup.identities;
            party.count = startup.count;
        }
    }
    if (!prepare_allocation(selection, sessionId, prepared, shared ? &party : nullptr)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=6 stage=allocation result=fail");
        return false;
    }
    middleware::bap::activity_host_manager::response::Response response{};
    if (!make_establish_response(sessionId, response)
        || !middleware::bap::activity_host_manager::response::encode_response(
            response, output, written)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=6 stage=encode result=fail");
        prepared = {};
        return false;
    }
    // Publish only scalar transaction data. Borrowed protobuf bytes never enter State.
    allocation = prepared;
    startupReservations = startup;
    hasAllocation = true;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_host_manager
