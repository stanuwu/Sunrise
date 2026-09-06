#include "activity_membership_push.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/settings.h"
#include "../../../../../middleware/bap/activity_message/replicate_membership.h"
#include "../../../../../middleware/encoding/byte_order.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/forced/activity_forced_destination.h"
#include "../../../../../state/activity_sdk/generated_world/runtime.h"
#include "../../../../../state/activity_sdk/runtime.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "../../../../gameplay/group/group_host_sessions.h"
#include "../../bap_connection_publication.h"
#include "activity_arrival.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace membership_message = middleware::bap::activity_message::replicate_membership;
namespace sdk = state::activity_sdk;

/** The one published member always occupies slot zero of both top-level masks. */
constexpr std::uint8_t kLocalMemberSlot = 0;
/** Descriptor fields reused by the matching remote activity-member row. */
constexpr std::size_t kDescriptorMachineOffset = 0;
constexpr std::size_t kDescriptorNetAddrOffset = 8;
/** The plain NetAddr keeps its UDP port low byte first at offset 4. */
constexpr std::size_t kDescriptorPortOffset = 4;
constexpr std::size_t kDescriptorSessionOffset = 110;
/** Standard FNV-1a constants mirror the client's process-session hash. */
constexpr std::uint64_t kFnv1aBasis = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnv1aPrime = 0x100000001B3ULL;
/** The binary names the Bubble Host process `bh64`. */
constexpr std::string_view kBubbleHostName{"bh64"};

/** Copies one ASCII string into a reflected signed-byte region. */
template <std::size_t Size>
[[nodiscard]] bool copy_string(std::string_view source,
                               std::array<std::int8_t, Size>& output) noexcept {
    output = {};
    if (source.size() >= output.size()) {
        return false;
    }
    for (std::size_t index = 0; index < source.size(); ++index) {
        output[index] = static_cast<std::int8_t>(source[index]);
    }
    return true;
}

/** @return FNV-1a of one ASCII string. */
[[nodiscard]] std::uint64_t hash_string(std::string_view value) noexcept {
    std::uint64_t hash = kFnv1aBasis;
    for (const char character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnv1aPrime;
    }
    return hash;
}

/** Builds the remote row from the same retained host and descriptor the region publishes. */
[[nodiscard]] bool
build_remote_member(const membership_message::CitizenAdvertisement& advertisement,
                    membership_message::RemoteViewMember& output) noexcept {
    output = {};
    server::gameplay::group::HostSessionBinding host{};
    if (!advertisement.present || advertisement.onlineSessionId == 0
        || !server::gameplay::group::host_session_for_activity(advertisement.onlineSessionId, host)
        || host.groupSessionId == 0 || host.generation == 0) {
        return false;
    }
    const auto descriptor = std::span(advertisement.descriptor);
    const std::uint64_t machineId = middleware::encoding::read_u64_le(
        descriptor.subspan<kDescriptorMachineOffset, middleware::encoding::kU64Size>());
    const std::uint64_t peerSessionId = middleware::encoding::read_u64_le(
        descriptor.subspan<kDescriptorSessionOffset, middleware::encoding::kU64Size>());
    if (machineId != host.groupSessionId || peerSessionId == 0
        || host.target.sessionId != advertisement.onlineSessionId
        || std::to_integer<std::uint8_t>(
               descriptor[kDescriptorNetAddrOffset + middleware::gameplay::descriptor::kNetAddrSize
                          - 1])
               > 5) {
        return false;
    }

    std::array<char, 128> processSession{};
    const int length = std::snprintf(processSession.data(),
                                     processSession.size(),
                                     "bh64@%016llX@%016llX",
                                     static_cast<unsigned long long>(host.target.sessionId),
                                     static_cast<unsigned long long>(host.groupSessionId));
    if (length <= 0 || static_cast<std::size_t>(length) >= processSession.size()) {
        return false;
    }
    const std::string_view processSessionView(processSession.data(),
                                              static_cast<std::size_t>(length));
    if (!copy_string(processSessionView, output.processSessionId)
        || !copy_string(kBubbleHostName, output.playerName)) {
        return false;
    }

    output.processSessionIdHash = hash_string(processSessionView);
    output.identity.memberKey = machineId;
    // A real slice-set index selects the client's direct security manager for this channel;
    // -1 selects the owner-backed one, which waits on a session a bubble host never gets.
    output.identity.field1 = advertisement.regionIndex;
    output.identity.field2 = -1;
    output.identity.field3 = host.generation;
    output.identity.accountSoid = host.target.sessionId;
    output.identity.field5 = host.groupSessionId;
    output.identity.field6 = peerSessionId;
    // The region's descriptor carries the plain bdCommonAddr form. The member row's channel is
    // secured by the client's direct security manager, which decodes only the direct form.
    const std::uint32_t address = middleware::encoding::read_u32_be(
        descriptor.subspan<kDescriptorNetAddrOffset, middleware::encoding::kU32Size>());
    const std::size_t portOffset = kDescriptorNetAddrOffset + kDescriptorPortOffset;
    const auto port =
        static_cast<std::uint16_t>(std::to_integer<unsigned>(descriptor[portOffset])
                                   | (std::to_integer<unsigned>(descriptor[portOffset + 1]) << 8U));
    middleware::gameplay::descriptor::write_direct_net_addr(address, port, output.address);
    output.present = true;
    return true;
}

/**
 * Binds the generated world one activity session's selected activity names.
 * @param source Exact activity session.
 * @param bindingGeneration Connection generation that binds it.
 * @param output Receives the bound generated-world view.
 * @return True when the selection resolves to authenticated live content.
 */
[[nodiscard]] bool
resolve_generated_world(const state::activity::SessionBinding& source,
                        std::uint64_t bindingGeneration,
                        sdk::generated_world::GeneratedWorldView& output) noexcept {
    const sdk::Snapshot catalog = sdk::snapshot();
    sdk::BoundView activityView{};
    const sdk::Selection selection{source, activity_link_count_locked(source), bindingGeneration};
    return catalog != nullptr
           && sdk::resolve(catalog, selection, activityView) == sdk::Status::ready
           && sdk::generated_world::resolve(activityView, output)
                  == sdk::generated_world::BindStatus::ready;
}

/** Binds the generated world of the connection's own activity session. */
[[nodiscard]] bool
resolve_generated_world(const Session& session,
                        sdk::generated_world::GeneratedWorldView& output) noexcept {
    return resolve_generated_world(
        session.activity.session, session.activity.bindingGeneration, output);
}

/** @return True only when extracted package data selects a private Bubble Host lane. */
[[nodiscard]] bool remote_member_allowed(const Session& session,
                                         std::int32_t effectiveRegion) noexcept {
    const core::settings::Settings& settings = core::settings::get();
    if (settings.client.regionPrivate || state::activity::forced::override_active()) {
        return true;
    }
    sdk::generated_world::GeneratedWorldView worldView{};
    bool isPublic = false;
    return resolve_generated_world(session, worldView)
           && sdk::generated_world::region_is_public(worldView, effectiveRegion, isPublic)
           && !isPublic;
}

/** @return One wire leg copied from a reported State leg, or an absent one. */
[[nodiscard]] membership_message::RegionLeg
mirror_leg(const state::activity::membership::RegionState& leg, bool reported) noexcept {
    membership_message::RegionLeg wire{};
    if (!reported) {
        return wire;
    }
    wire.sliceSetIndex = leg.sliceSetIndex;
    wire.sliceSetHash = leg.hash;
    wire.regionIndex = leg.index;
    wire.publicState = leg.publicState;
    wire.auxState = leg.auxState;
    wire.present = true;
    return wire;
}

/**
 * Maps a lock-consistent State snapshot into the fixed Middleware schema.
 * @param session Exact ActivityClient owner, which selects the authored region policy.
 * @param mutation Prepared membership operation this body publishes.
 * @param activityHostId Activity session soid this link answers on, published as identity field 4.
 * @return Membership encoder input, without the citizen advertisement.
 */
[[nodiscard]] membership_message::MembershipSnapshot
make_base_snapshot(const Session& session,
                   const state::activity::membership::PendingMutation& mutation,
                   std::uint64_t activityHostId) noexcept {
    const state::activity::membership::Snapshot& snapshot = mutation.snapshot;
    membership_message::MembershipSnapshot wire{};
    wire.activityHostId = activityHostId;
    wire.identity.memberKey = snapshot.identity.memberKey;
    wire.identity.field1 = snapshot.identity.smallOpaque;
    wire.identity.field2 = snapshot.identity.signedOpaque;
    wire.identity.field3 = snapshot.identity.joinIdentity;
    wire.identity.accountSoid = snapshot.identity.accountSoid;
    wire.identity.field5 = snapshot.identity.opaqueSoid;
    wire.identity.field6 = snapshot.identity.secondaryOpaque;
    wire.spawn.state = snapshot.spawn.state;
    wire.spawn.opaqueByte = snapshot.spawn.opaqueByte;
    wire.spawn.opaqueValue = snapshot.spawn.opaqueValue;
    wire.teleport.state = snapshot.teleport.state;
    wire.teleport.token = snapshot.teleport.token;
    wire.teleport.sliceSetIndex = snapshot.teleport.sliceSetIndex;
    wire.teleport.sliceSetHash = snapshot.teleport.sliceSetHash;
    // The member's own region legs are mirrored back exactly as it reported them.
    wire.currentLeg = mirror_leg(snapshot.currentLeg, snapshot.hasCurrentLeg);
    wire.pendingLeg = mirror_leg(snapshot.pendingLeg, snapshot.hasPendingLeg);
    wire.revision = snapshot.revision;
    wire.epoch = snapshot.epoch;
    wire.transitionToken = snapshot.transitionToken;
    static_cast<void>(region_publicity_mask(session, wire.regionPublicMask));
    return wire;
}

/**
 * Adds the host directory a private link owes on top of the mapped fields.
 * The client reads this table to find which host owns a region. One filled row only answers for
 * the region it is in, and a fast travel drops that host before it looks for the target.
 *
 * @param session Exact ActivityClient owner of the membership body.
 * @param mutation Prepared membership operation, whose region this body publishes.
 * @param retains Cleared, then receives one retained host row per advertised region.
 * @param activityHostId Activity session soid this link answers on, published as identity field 4.
 * @return Whole current membership encoder input.
 */
[[nodiscard]] membership_message::MembershipSnapshot
make_wire_snapshot(const Session& session,
                   const state::activity::membership::PendingMutation& mutation,
                   AdvertisementRetains& retains,
                   std::uint64_t activityHostId) noexcept {
    retains = {};
    membership_message::MembershipSnapshot wire =
        make_base_snapshot(session, mutation, activityHostId);
    if (session.activity.role != ActivityClientRole::privateCurrent
        || !state::activity::binding_matches(session.activity.source)) {
        return wire;
    }
    // The region this body is about to commit, not the one State still holds. Staging runs before
    // the commit, so the region just left would leave the pending record empty for good.
    EffectiveRegion region = private_planned_region(mutation, session.activity.source);
    // `directory_regions` skips the published region's own bubble. So a teleport into that bubble
    // would leave its record naming the region just left, and the client would never precache
    // the target. Name the target on that record instead.
    namespace tables = middleware::content::packages::tables;
    const std::int32_t teleportRegion = mutation.snapshot.teleport.sliceSetIndex;
    if (teleportRegion >= 0 && region.index >= 0 && teleportRegion != region.index
        && static_cast<std::uint32_t>(teleportRegion) / tables::kSliceSetIndexFactor
               == static_cast<std::uint32_t>(region.index) / tables::kSliceSetIndexFactor) {
        region.index = teleportRegion;
    }
    // A private region has no citizen join. Its region remains self-hosted while the separate
    // Bubble Host member supplies the gameplay view used by the local simulation.
    if (private_region(session, region.index)) {
        wire.selfHosted = true;
        wire.selfHostedRegion = region.index;
        membership_message::CitizenAdvertisement host{};
        std::uint64_t hostGeneration = 0;
        server::gameplay::build_private_host_advertisement(
            session.activity.source, region.index, kLocalMemberSlot, host, hostGeneration);
        if (hostGeneration != 0 && build_remote_member(host, wire.remoteViewMember)) {
            retains.hostGenerations[retains.count++] = hostGeneration;
        } else if (hostGeneration != 0) {
            server::gameplay::group::release_host_session(hostGeneration);
        }
        return wire;
    }
    std::array<std::int32_t, membership_message::kCitizenCapacity> regions{};
    std::size_t regionCount = 0;
    directory_regions(session.activity.source, region.index, regions, regionCount);
    for (std::size_t entry = 0; entry < regionCount; ++entry) {
        membership_message::CitizenAdvertisement candidate{};
        std::uint64_t hostGeneration = 0;
        // The published region is entry zero and is the one worth a log line. Reporting each of
        // the others would put one line per region on every push.
        if (entry == 0) {
            server::gameplay::build_advertisement(session.activity.source,
                                                  regions[entry],
                                                  region.reported
                                                      ? server::gameplay::RegionSource::reported
                                                      : server::gameplay::RegionSource::arrival,
                                                  kLocalMemberSlot,
                                                  candidate,
                                                  hostGeneration);
        } else {
            server::gameplay::build_directory_entry(session.activity.source,
                                                    regions[entry],
                                                    kLocalMemberSlot,
                                                    candidate,
                                                    hostGeneration);
        }
        if (!candidate.present) {
            continue;
        }
        if (entry == 0 && remote_member_allowed(session, regions[entry])) {
            static_cast<void>(build_remote_member(candidate, wire.remoteViewMember));
        }
        wire.citizens[wire.citizenCount] = candidate;
        ++wire.citizenCount;
        retains.hostGenerations[retains.count] = hostGeneration;
        ++retains.count;
    }
    return wire;
}

/**
 * Reports the host directory one body carries.
 * A body naming one region and a body naming eight are the same size on the roster line.
 * The fast-travel target is exactly the row that is hard to prove present.
 * @param wire Encoded snapshot whose directory this reports.
 */
void report_directory(const membership_message::MembershipSnapshot& wire) noexcept {
    if (wire.citizenCount == 0) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=gameplay stage=directory result=ok regions=%u published=%d last=%d",
                      static_cast<unsigned>(wire.citizenCount),
                      wire.citizens[0].regionIndex,
                      wire.citizens[wire.citizenCount - 1U].regionIndex);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Tests whether the installed packages author one region as private. */
bool private_region(const state::activity::SessionBinding& source,
                    std::uint64_t bindingGeneration,
                    std::int32_t region) noexcept {
    sdk::generated_world::GeneratedWorldView worldView{};
    bool isPublic = false;
    return region >= 0 && resolve_generated_world(source, bindingGeneration, worldView)
           && sdk::generated_world::region_is_public(worldView, region, isPublic) && !isPublic;
}

/** Same test for the connection's own activity session. */
bool private_region(const Session& session, std::int32_t region) noexcept {
    return private_region(session.activity.session, session.activity.bindingGeneration, region);
}

bool region_publicity(const Session& session, std::int32_t region, bool& isPublic) noexcept {
    isPublic = false;
    sdk::generated_world::GeneratedWorldView worldView{};
    return region >= 0 && resolve_generated_world(session, worldView)
           && sdk::generated_world::region_is_public(worldView, region, isPublic);
}

/** Publicity is authored per bubble, so a bubble's state zero answers for all 8 of its states. */
bool region_publicity_mask(const Session& session, std::uint64_t& mask) noexcept {
    mask = 0;
    sdk::generated_world::GeneratedWorldView worldView{};
    if (!resolve_generated_world(session, worldView)) {
        return false;
    }
    for (std::size_t record = 0; record < membership_message::kRegionRecordCount; ++record) {
        const auto region =
            static_cast<std::int32_t>(record) * membership_message::kRegionStateCount;
        bool isPublic = false;
        if (sdk::generated_world::region_is_public(worldView, region, isPublic) && isPublic) {
            mask |= std::uint64_t{1} << record;
        }
    }
    return true;
}

/** Reports whether the citizen advertisement for one region can be built now. */
server::gameplay::AdvertisementState region_advertisement(const Session& session,
                                                          std::int32_t region) noexcept {
    if (private_region(session, region)) {
        server::gameplay::group::HostSessionBinding host{};
        if (server::gameplay::private_host_session(session.activity.source, host)) {
            return server::gameplay::AdvertisementState::ready;
        }
        // A public region claims its row when queried. Claim here too: a walk sends no join.
        // The claim must not allocate: this runs inside a staged push, and the allocation's
        // state revision bump retires the transaction that push is already carrying.
        return server::gameplay::claim_private_host_session(session.activity.source, region)
                   ? server::gameplay::AdvertisementState::ready
                   : server::gameplay::AdvertisementState::pending;
    }
    return server::gameplay::advertisement_state(session.activity.source, region);
}

/** Appends one current membership svc9 notification and advances its local nonce. */
bool append_membership_notification(Scratch& scratch,
                                    Session& session,
                                    const activity_message::ActivityPlan& activity,
                                    std::span<const std::byte, state::kAesKeySize> key,
                                    std::array<std::byte, state::kBapNonceSize>& nonce,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    bool suppressUnchanged,
                                    bool* suppressed) noexcept {
    if (suppressed != nullptr) {
        *suppressed = false;
    }
    if (written > response.size() || !activity.membershipMutation.hasSnapshot) {
        return false;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    AdvertisementRetains retains{};
    const membership_message::MembershipSnapshot snapshot = make_wire_snapshot(
        session, activity.membershipMutation, retains, session.activity.session.sessionId);
    bool encoded = membership_message::encode_replicate_membership(
        snapshot, scratch.responseBody, messageSize);
    if (!encoded) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=activity stage=membership result=encode_refused");
    }
    // The client applies one membership update per revision and drops repeats, so a body
    // identical to the last delivered one is not sent again.
    if (encoded && suppressUnchanged
        && repeats_delivered_membership_body(session,
                                             std::span(scratch.responseBody).first(messageSize))) {
        for (std::size_t entry = 0; entry < retains.count; ++entry) {
            server::gameplay::group::release_host_session(retains.hostGenerations[entry]);
        }
        SecureZeroMemory(scratch.responseBody.data(), membership_message::encoded_size(snapshot));
        SecureZeroMemory(&initialNonce, sizeof initialNonce);
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=activity stage=membership result=unchanged");
        if (suppressed != nullptr) {
            *suppressed = true;
        }
        return false;
    }
    encoded = encoded
              && append_notification_frame(scratch,
                                           session.activity.session.sessionId,
                                           membership_message::kMessageType,
                                           std::span(scratch.responseBody).first(messageSize),
                                           key,
                                           nonce,
                                           response,
                                           written);
    if (encoded) {
        stage_membership_body_record(session, std::span(scratch.responseBody).first(messageSize));
    }
    SecureZeroMemory(scratch.responseBody.data(), membership_message::encoded_size(snapshot));
    if (encoded) {
        report_directory(snapshot);
        stage_activity_advertisement(session, retains);
        middleware::secure_channel::advance_nonce(nonce);
    } else {
        for (std::size_t entry = 0; entry < retains.count; ++entry) {
            server::gameplay::group::release_host_session(retains.hostGenerations[entry]);
        }
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

/** Appends the membership body a public-target join answers. */
bool append_join_membership_notification(Scratch& scratch,
                                         const Session& session,
                                         const activity_message::ActivityPlan& activity,
                                         std::span<const std::byte, state::kAesKeySize> key,
                                         std::array<std::byte, state::kBapNonceSize>& nonce,
                                         std::span<std::byte> response,
                                         std::size_t& written) noexcept {
    if (written > response.size() || !activity.membershipMutation.hasSnapshot
        || activity.sessionId == state::activity::kAbsentSessionId) {
        return false;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    const membership_message::MembershipSnapshot snapshot =
        make_base_snapshot(session, activity.membershipMutation, activity.sessionId);
    const bool bodyEncoded = membership_message::encode_replicate_membership(
        snapshot, scratch.responseBody, messageSize);
    if (!bodyEncoded) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=activity stage=membership result=encode_refused");
    }
    const bool encoded =
        bodyEncoded
        && append_notification_frame(scratch,
                                     activity.sessionId,
                                     membership_message::kMessageType,
                                     std::span(scratch.responseBody).first(messageSize),
                                     key,
                                     nonce,
                                     response,
                                     written);
    SecureZeroMemory(scratch.responseBody.data(), membership_message::encoded_size(snapshot));
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
    } else {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

} // namespace sunrise::server::bap::encrypted::push::activity
