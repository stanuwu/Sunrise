#include "gameplay_advertisement.h"

#include <atomic>
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

/** @return A stable nonzero region-specific copy of one process identity field. */
[[nodiscard]] std::uint64_t region_identity(std::uint64_t base, std::int32_t regionIndex) noexcept {
    const auto region = static_cast<std::uint64_t>(static_cast<std::uint32_t>(regionIndex));
    const std::uint64_t derived = base ^ (kRegionStride * (region + 1U));
    return derived == 0 ? kRegionStride : derived;
}

/** @return Group-session key carried by one region's descriptor. */
[[nodiscard]] std::uint64_t region_machine_id(std::int32_t regionIndex) noexcept {
    return region_identity(endpoint::identity().machineId, regionIndex);
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
    /** Stable log labels follow the Skip enumeration's ordinal order. */
    static constexpr const char* kReasons[] = {"none",
                                               "not_ready",
                                               "no_region",
                                               "slot_range",
                                               "no_source",
                                               "descriptor",
                                               "no_host_session",
                                               "host_session_conflict",
                                               "host_session_full"};
    report(core::log::Level::info,
           "ev=gameplay stage=advertise result=skip reason=%s region=%d region_source=%s",
           kReasons[static_cast<std::size_t>(skip)],
           static_cast<int>(regionIndex),
           source);
}

/** @return Encodable ambassador slot that differs from the joining client's slot. */
[[nodiscard]] std::uint8_t ambassador_slot(std::uint8_t localMemberSlot) noexcept {
    return localMemberSlot == 0 ? 1 : 0;
}

/** Builds one source-bound candidate and retains its host generation on success. */
[[nodiscard]] Skip build_candidate(const state::activity::SessionBinding& source,
                                   std::int32_t regionIndex,
                                   std::uint8_t localMemberSlot,
                                   message::CitizenAdvertisement& candidate,
                                   std::uint64_t& hostGeneration) noexcept {
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
    const state::gameplay::Endpoint advertised = endpoint::advertised();
    group::HostSessionBinding host{};
    switch (
        group::request_host_session(region_machine_id(regionIndex), source, regionIndex, host)) {
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
    if (!group::retain_host_session(host.generation)) {
        return Skip::noHostSession;
    }

    middleware::gameplay::descriptor::JoinEndpoint join{};
    join.address = advertised.address;
    join.port = advertised.port;
    join.machineId = host.groupSessionId;
    join.onlineSessionId = region_identity(identity.onlineSessionId, regionIndex);
    if (!middleware::gameplay::descriptor::build(join, candidate.descriptor)) {
        group::release_host_session(host.generation);
        candidate = {};
        return Skip::descriptor;
    }

    candidate.onlineSessionId = host.target.sessionId;
    candidate.regionIndex = regionIndex;
    candidate.ambassadorSlot = ambassador_slot(localMemberSlot);
    candidate.present = true;
    hostGeneration = host.generation;
    return Skip::none;
}

} // namespace

/** Builds a citizen advertisement from one exact source activity generation. */
void build_advertisement(const state::activity::SessionBinding& source,
                         std::int32_t regionIndex,
                         RegionSource regionSource,
                         std::uint8_t localMemberSlot,
                         message::CitizenAdvertisement& output,
                         std::uint64_t& hostGeneration) noexcept {
    const Skip skip = build_candidate(source, regionIndex, localMemberSlot, output, hostGeneration);
    report_outcome(skip, regionIndex, regionSource, output.ambassadorSlot);
}

/** Claims a missing host row and reports whether its advertisement is ready. */
AdvertisementState advertisement_state(const state::activity::SessionBinding& source,
                                       std::int32_t regionIndex) noexcept {
    message::CitizenAdvertisement candidate{};
    std::uint64_t generation = 0;
    const Skip skip =
        build_candidate(source, regionIndex, kQueriedMemberSlot, candidate, generation);
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

/** Source-less publishers remain wire-silent and do not claim a host row. */
void build_advertisement(std::int32_t regionIndex,
                         RegionSource regionSource,
                         std::uint8_t localMemberSlot,
                         message::CitizenAdvertisement& output) noexcept {
    static_cast<void>(localMemberSlot);
    output = {};
    report_outcome(Skip::noSource, regionIndex, regionSource, 0);
}

/** Source-less readiness queries remain absent and do not claim a host row. */
AdvertisementState advertisement_state(std::int32_t regionIndex) noexcept {
    static_cast<void>(regionIndex);
    return AdvertisementState::absent;
}

} // namespace sunrise::server::gameplay
