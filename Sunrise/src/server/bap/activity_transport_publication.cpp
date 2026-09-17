#include "activity_transport_publication.h"

#include <array>
#include <cstddef>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/gameplay/descriptor/net_addr.h"
#include "../../state/account/public_profiles.h"
#include "../../state/activity/membership/activity_transport_fields.h"
#include "../../state/activity/reservations/runtime.h"
#include "internal.h"

namespace sunrise::server::bap {
namespace {
/**
 * Rebuilds the published transport mirror for one member key from every live connection that
 * holds it. The caller holds the BAP lock. A member with no live carrier, or whose live
 * connections disagree after normalisation, publishes nothing at all so the type-12 row, the
 * join descriptor and the group snapshot all resolve the same unambiguous carrier. This is the
 * same rule `find_transport_locked` applies below.
 */
void refresh_member_identity_locked(std::uint64_t memberKey) noexcept {
    namespace membership = state::activity::membership;
    namespace descriptor = middleware::gameplay::descriptor;
    if (memberKey == 0) {
        return;
    }
    const Session* selected{};
    // A link that has reported its address but not yet its flags must not withdraw the ready
    // state the member's other link carries, so the scan retains a live link that reports flags.
    const Session* flagged{};
    std::array<std::byte, descriptor::kNetAddrSize> agreed{};
    auto rule = descriptor::NetAddrNormalisation::unavailable;
    for (const auto& candidate : sessions()) {
        if (candidate.id == 0 || !candidate.authenticated
            || candidate.activity.role == ActivityClientRole::none
            || candidate.activity.bindingGeneration == 0 || candidate.activityMemberKey != memberKey
            || candidate.activityJoinGeneration != candidate.activity.bindingGeneration
            || (!candidate.activityTransport.hasAddress
                && !candidate.activityTransport.hasAlternate)) {
            continue;
        }
        std::array<std::byte, descriptor::kNetAddrSize> chosen{};
        const auto candidateRule = descriptor::normalize_net_addr_ipv4(
            candidate.activityTransport.address, candidate.activityTransport.alternate, chosen);
        if (candidateRule == descriptor::NetAddrNormalisation::unavailable) {
            continue;
        }
        if (selected != nullptr && chosen != agreed) {
            selected = nullptr;
            break;
        }
        selected = &candidate;
        if (candidate.activityTransport.hasFlags) {
            flagged = &candidate;
        }
        agreed = chosen;
        rule = candidateRule;
    }
    if (selected == nullptr) {
        membership::forget_transport_fields(memberKey);
        return;
    }
    membership::TransportFields fields{};
    fields.address = selected->activityTransport.address;
    fields.addressAlt = selected->activityTransport.alternate;
    fields.flags = flagged != nullptr ? flagged->activityTransport.flags : 0;
    fields.hasFlags = flagged != nullptr;
    fields.hasAddress = selected->activityTransport.hasAddress;
    fields.hasAddressAlt = selected->activityTransport.hasAlternate;
    if (!membership::observe_transport_fields(memberKey,
                                              state::account_primary_soid(selected->accountHandle),
                                              selected->activityCharacterSoid,
                                              fields)) {
        return;
    }
    // One line per change, not per push.
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::array<char, descriptor::kNetAddrSize * 2 + 1> hex{};
    for (std::size_t index = 0; index < agreed.size(); ++index) {
        const auto value = std::to_integer<unsigned>(agreed[index]);
        hex[index * 2] = kDigits[(value >> 4U) & 0xFU];
        hex[index * 2 + 1] = kDigits[value & 0xFU];
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=descriptor stage=publish result=identity peer=0x%016llX family=%s bytes=%s",
        static_cast<unsigned long long>(memberKey),
        descriptor::net_addr_normalisation_name(rule),
        hex.data());
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}
} // namespace

void publish_activity_transport(
    Session& session,
    std::uint64_t bindingGeneration,
    const middleware::bap::activity_message::TransportReport& report) noexcept {
    if (session.id == 0 || bindingGeneration == 0 || session.activityMemberKey == 0
        || session.activity.bindingGeneration != bindingGeneration
        || session.activityJoinGeneration != bindingGeneration
        || session.activity.role == ActivityClientRole::none) {
        return;
    }
    if (middleware::bap::activity_message::merge_transport(session.activityTransport, report)) {
        // The type-12 row, join descriptor and group snapshot all name this peer by the same 86
        // bytes, so the mirror is rebuilt from the retained reports and not from the sparse
        // delta that changed one of them.
        refresh_member_identity_locked(session.activityMemberKey);
        state::account::profiles::service_changed(session.accountHandle);
        state::activity::reservations::invalidate_owner(
            state::account_primary_soid(session.accountHandle));
    }
}

void clear_activity_transport(Session& session) noexcept {
    const auto memberKey = session.activityMemberKey;
    const bool retained =
        session.activityTransport != middleware::bap::activity_message::TransportReport{};
    session.activityTransport = {};
    // Rebuilt, not simply forgotten: a member whose private link ends while its public link is
    // still up keeps the surviving link's carrier, and a rejoin under the same member key can
    // never read the previous join's.
    refresh_member_identity_locked(memberKey);
    if (!retained) {
        return;
    }
    state::account::profiles::service_changed(session.accountHandle);
    state::activity::reservations::invalidate_owner(
        state::account_primary_soid(session.accountHandle));
}

namespace {
bool find_transport_locked(std::uint64_t accountSoid,
                           std::uint64_t characterSoid,
                           std::uint64_t memberKey,
                           middleware::bap::activity_message::TransportReport& report) noexcept {
    report = {};
    if (accountSoid == 0 || characterSoid == 0) {
        return false;
    }
    bool found{};
    for (const auto& session : sessions()) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0 || session.activityMemberKey == 0
            || (memberKey != 0 && session.activityMemberKey != memberKey)
            || session.activityJoinGeneration != session.activity.bindingGeneration
            || session.activityCharacterSoid != characterSoid
            || state::account_primary_soid(session.accountHandle) != accountSoid
            || (!session.activityTransport.hasAddress && !session.activityTransport.hasAlternate)) {
            continue;
        }
        namespace descriptor = middleware::gameplay::descriptor;
        auto candidate = session.activityTransport;
        std::array<std::byte, descriptor::kNetAddrSize> address{};
        if (descriptor::normalize_net_addr_ipv4(candidate.address, candidate.alternate, address)
            == descriptor::NetAddrNormalisation::unavailable) {
            continue;
        }
        // Private and public activity links may publish different carrier forms. Only usable
        // native addresses participate in conflict detection, after equivalent forms normalize.
        candidate.address = candidate.alternate = address;
        candidate.hasAddress = candidate.hasAlternate = true;
        if (found && report.address != candidate.address) {
            report = {};
            return false;
        }
        // Same rule as the mirror above: an address-only report keeps the flags already found.
        if (found && report.hasFlags && !candidate.hasFlags) {
            candidate.hasFlags = true;
            candidate.flags = report.flags;
        }
        report = candidate;
        found = true;
    }
    return found;
}
} // namespace

bool published_activity_transport_locked(
    std::uint64_t accountSoid,
    std::uint64_t characterSoid,
    std::uint64_t memberKey,
    middleware::bap::activity_message::TransportReport& report) noexcept {
    report = {};
    return memberKey != 0 && find_transport_locked(accountSoid, characterSoid, memberKey, report);
}

bool published_character_transport_locked(
    std::uint64_t accountSoid,
    std::uint64_t characterSoid,
    middleware::bap::activity_message::TransportReport& report) noexcept {
    return find_transport_locked(accountSoid, characterSoid, 0, report);
}
} // namespace sunrise::server::bap
