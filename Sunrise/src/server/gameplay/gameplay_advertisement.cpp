#include "gameplay_advertisement.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "../../middleware/gameplay/descriptor/join_descriptor.h"
#include "endpoint/gameplay_endpoint.h"
#include "gameplay_log.h"
#include "group/group_host_sessions.h"

namespace sunrise::server::gameplay {

namespace {

namespace message = middleware::bap::activity_message::replicate_membership;

/** Member-slot fields are 6 bits at bias 1, so the slot itself cannot exceed 62. */
constexpr std::uint8_t kMaximumMemberSlot = 62;
/** Slot a readiness query stands in with. Every publisher names the one member it publishes. */
constexpr std::uint8_t kQueriedMemberSlot = 0;
/** Odd multiplier that spreads one region index across the whole 64-bit space. */
constexpr std::uint64_t kRegionStride = 0x9E3779B97F4A7C15ULL;
/** Moves a small revision clear of the session-id product's low bits before they are combined. */
constexpr int kRevisionRotation = 17;

/** @return A stable nonzero region-specific copy of one process identity field. */
[[nodiscard]] std::uint64_t region_identity(std::uint64_t base, std::int32_t regionIndex) noexcept {
    const auto region = static_cast<std::uint64_t>(static_cast<std::uint32_t>(regionIndex));
    const std::uint64_t derived = base ^ (kRegionStride * (region + 1U));
    return derived == 0 ? kRegionStride : derived;
}

/** Host rows retain an exact source generation, so their identities must use that scope too. */
[[nodiscard]] std::uint64_t
source_identity(std::uint64_t base, const state::activity::SessionBinding& source) noexcept {
    const auto derived = base ^ (source.sessionId * kRegionStride)
                         ^ std::rotl(source.createdRevision, kRevisionRotation);
    return derived == 0 ? kRegionStride : derived;
}

/** @return Group-session key carried by one source generation's regional descriptor. */
[[nodiscard]] std::uint64_t region_machine_id(const state::activity::SessionBinding& source,
                                              std::int32_t regionIndex) noexcept {
    return region_identity(source_identity(endpoint::identity().machineId, source), regionIndex);
}

/** No outcome packs to this, so the first advertisement always reports. */
constexpr std::uint64_t kNoOutcome = ~0ULL;
/** Ambassador slot sits above the region index, which holds the low 32 bits. */
constexpr unsigned kSlotShift = 32;
/** Skip reason sits above the ambassador slot. */
constexpr unsigned kReasonShift = 40;
/** Region source sits above the skip reason. */
constexpr unsigned kSourceShift = 48;

/** Why an advertisement was not built. */
enum class Skip : std::uint64_t {
    none,
    notReady,
    noRegion,
    slotRange,
    noSource,
    descriptor,
    noHostSession,
    hostSessionConflict,
    hostSessionFull,
};

std::atomic<std::uint64_t> g_reported{kNoOutcome};

/** Log names for RegionSource, in its declaration order. */
constexpr const char* kSources[] = {"reported", "arrival"};
/** Log names for Skip, in its declaration order. */
constexpr const char* kReasons[] = {"none",
                                    "not_ready",
                                    "no_region",
                                    "slot_range",
                                    "no_source",
                                    "descriptor",
                                    "no_host_session",
                                    "host_session_conflict",
                                    "host_session_full"};

/** Reports one changed advertisement outcome. */
void report_outcome(Skip skip,
                    std::int32_t regionIndex,
                    RegionSource regionSource,
                    std::uint8_t ambassadorSlot) noexcept {
    const auto outcome = static_cast<std::uint64_t>(static_cast<std::uint32_t>(regionIndex))
                         | (static_cast<std::uint64_t>(ambassadorSlot) << kSlotShift)
                         | (static_cast<std::uint64_t>(skip) << kReasonShift)
                         | (static_cast<std::uint64_t>(regionSource) << kSourceShift);
    if (g_reported.exchange(outcome, std::memory_order_relaxed) == outcome) {
        return;
    }
    const char* source = kSources[static_cast<std::size_t>(regionSource)];
    if (skip == Skip::none) {
        report(core::log::Level::info,
               "ev=gameplay stage=advertise result=ok region=%d region_source=%s "
               "ambassador_slot=%u",
               static_cast<int>(regionIndex),
               source,
               static_cast<unsigned>(ambassadorSlot));
        return;
    }
    report(core::log::Level::info,
           "ev=gameplay stage=advertise result=skip reason=%s region=%d region_source=%s",
           kReasons[static_cast<std::size_t>(skip)],
           static_cast<int>(regionIndex),
           source);
}

/** @return Encodable ambassador slot that differs from the joining client's slot. */
[[nodiscard]] std::uint8_t ambassador_slot(std::uint8_t localMemberSlot) noexcept {
    return localMemberSlot == 1 ? 0 : 1;
}

/** Encodes one already allocated host row for the region its member currently serves. */
[[nodiscard]] bool encode_candidate(const group::HostSessionBinding& host,
                                    std::uint64_t onlineSessionId,
                                    std::int32_t regionIndex,
                                    std::uint8_t localMemberSlot,
                                    message::CitizenAdvertisement& candidate,
                                    std::uint64_t& hostGeneration) noexcept {
    candidate = {};
    hostGeneration = 0;
    if (host.groupSessionId == 0 || host.target.sessionId == 0 || onlineSessionId == 0
        || !group::retain_host_session(host.generation)) {
        return false;
    }
    const state::gameplay::Endpoint advertised = endpoint::advertised();
    middleware::gameplay::descriptor::JoinEndpoint join{};
    join.address = advertised.address;
    join.port = host.port != 0 ? host.port : advertised.port;
    join.machineId = host.groupSessionId;
    join.onlineSessionId = onlineSessionId;
    if (!middleware::gameplay::descriptor::build(join, candidate.descriptor)) {
        group::release_host_session(host.generation);
        return false;
    }
    candidate.onlineSessionId = host.target.sessionId;
    candidate.regionIndex = regionIndex;
    candidate.ambassadorSlot = ambassador_slot(localMemberSlot);
    candidate.present = true;
    hostGeneration = host.generation;
    return true;
}

/** Builds one source-bound candidate and retains its host generation on success. */
[[nodiscard]] Skip build_candidate(const state::activity::SessionBinding& source,
                                   std::int32_t regionIndex,
                                   std::uint8_t localMemberSlot,
                                   message::CitizenAdvertisement& candidate,
                                   std::uint64_t& hostGeneration,
                                   bool publicRegion) noexcept {
    candidate = {};
    hostGeneration = 0;
    if (!endpoint::ready()) {
        return Skip::notReady;
    }
    if (regionIndex < 0) {
        return Skip::noRegion;
    }
    if (localMemberSlot > kMaximumMemberSlot) {
        return Skip::slotRange;
    }

    const endpoint::Identity identity = endpoint::identity();
    group::HostSessionBinding host{};
    switch (group::request_host_session(
        region_machine_id(source, regionIndex), source, regionIndex, host, publicRegion)) {
    case group::HostSessionState::ready:
        break;
    case group::HostSessionState::pending:
        return Skip::noHostSession;
    case group::HostSessionState::conflict:
        return Skip::hostSessionConflict;
    case group::HostSessionState::full:
        return Skip::hostSessionFull;
    case group::HostSessionState::absent:
    default:
        return Skip::noSource;
    }
    if (!encode_candidate(
            host,
            region_identity(source_identity(identity.onlineSessionId, host.source), regionIndex),
            regionIndex,
            localMemberSlot,
            candidate,
            hostGeneration)) {
        return Skip::noHostSession;
    }
    return Skip::none;
}

} // namespace

/** Builds a citizen advertisement from one exact source activity generation. */
void build_advertisement(const state::activity::SessionBinding& source,
                         std::int32_t regionIndex,
                         RegionSource regionSource,
                         std::uint8_t localMemberSlot,
                         message::CitizenAdvertisement& output,
                         std::uint64_t& hostGeneration,
                         bool publicRegion) noexcept {
    const Skip skip =
        build_candidate(source, regionIndex, localMemberSlot, output, hostGeneration, publicRegion);
    report_outcome(skip, regionIndex, regionSource, output.ambassadorSlot);
}

/** Builds one further directory entry from the same source, without reporting an outcome. */
void build_directory_entry(const state::activity::SessionBinding& source,
                           std::int32_t regionIndex,
                           std::uint8_t localMemberSlot,
                           message::CitizenAdvertisement& output,
                           std::uint64_t& hostGeneration,
                           bool publicRegion) noexcept {
    static_cast<void>(build_candidate(
        source, regionIndex, localMemberSlot, output, hostGeneration, publicRegion));
}

/** Claims a missing host row and reports whether its advertisement is ready. */
AdvertisementState advertisement_state(const state::activity::SessionBinding& source,
                                       std::int32_t regionIndex,
                                       bool publicRegion) noexcept {
    message::CitizenAdvertisement candidate{};
    std::uint64_t generation = 0;
    const Skip skip = build_candidate(
        source, regionIndex, kQueriedMemberSlot, candidate, generation, publicRegion);
    if (generation != 0) {
        group::release_host_session(generation);
    }
    switch (skip) {
    case Skip::none:
        return AdvertisementState::ready;
    case Skip::noHostSession:
        return AdvertisementState::pending;
    default:
        return AdvertisementState::absent;
    }
}

/** Claims the region's host row and completes a pending allocation at once. */
void complete_host_session(const state::activity::SessionBinding& source,
                           std::int32_t regionIndex,
                           bool publicRegion) noexcept {
    if (advertisement_state(source, regionIndex, publicRegion) == AdvertisementState::pending) {
        group::allocate_claimed_host_sessions();
    }
}

/** Starts the private activity's stable logical Bubble Host and completes its allocation. */
bool complete_private_host_session(const state::activity::SessionBinding& source,
                                   std::int32_t initialRegion) noexcept {
    const endpoint::Identity identity = endpoint::identity();
    if (!endpoint::ready() || identity.machineId == 0 || initialRegion < 0) {
        return false;
    }
    group::HostSessionBinding host{};
    const group::HostSessionState state = group::request_host_session(
        source_identity(identity.machineId, source), source, initialRegion, host);
    if (state == group::HostSessionState::pending) {
        group::allocate_claimed_host_sessions();
    } else if (state != group::HostSessionState::ready) {
        return false;
    }
    return private_host_session(source, host);
}

/** Claims the private activity's Bubble Host row without allocating it. */
bool claim_private_host_session(const state::activity::SessionBinding& source,
                                std::int32_t initialRegion) noexcept {
    const endpoint::Identity identity = endpoint::identity();
    if (!endpoint::ready() || identity.machineId == 0 || initialRegion < 0) {
        return false;
    }
    group::HostSessionBinding host{};
    // A claimed row stays pending until the pump service allocates it, so a pending claim is not
    // an error: the next advertisement finds the row ready.
    return group::request_host_session(
               source_identity(identity.machineId, source), source, initialRegion, host)
               == group::HostSessionState::ready
           && private_host_session(source, host);
}

/** Copies the private activity's stable logical Bubble Host row. */
bool private_host_session(const state::activity::SessionBinding& source,
                          group::HostSessionBinding& output) noexcept {
    const endpoint::Identity identity = endpoint::identity();
    return identity.machineId != 0
           && group::host_session_for_source_group(
               source, source_identity(identity.machineId, source), output);
}

/** Builds the private logical host's remote-member descriptor for its current region. */
void build_private_host_advertisement(const state::activity::SessionBinding& source,
                                      std::int32_t regionIndex,
                                      std::uint8_t localMemberSlot,
                                      message::CitizenAdvertisement& output,
                                      std::uint64_t& hostGeneration) noexcept {
    output = {};
    hostGeneration = 0;
    const endpoint::Identity identity = endpoint::identity();
    group::HostSessionBinding host{};
    if (!endpoint::ready() || regionIndex < 0 || localMemberSlot > kMaximumMemberSlot
        || identity.onlineSessionId == 0 || !private_host_session(source, host)
        || !encode_candidate(host,
                             source_identity(identity.onlineSessionId, source),
                             regionIndex,
                             localMemberSlot,
                             output,
                             hostGeneration)) {
        return;
    }
}

/** Source-less publishers remain wire-silent and do not claim a host row. */
void build_advertisement(std::int32_t regionIndex,
                         RegionSource regionSource,
                         std::uint8_t,
                         message::CitizenAdvertisement& output) noexcept {
    output = {};
    report_outcome(Skip::noSource, regionIndex, regionSource, 0);
}

/** Source-less readiness queries remain absent and do not claim a host row. */
AdvertisementState advertisement_state(std::int32_t) noexcept {
    return AdvertisementState::absent;
}

} // namespace sunrise::server::gameplay
