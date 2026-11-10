#pragma once

#include <cstdint>

#include "../../middleware/bap/activity_message/replicate_membership.h"
#include "../../state/activity/definition.h"
#include "group/group_host_sessions.h"

namespace sunrise::server::gameplay {

/** Where an advertised region index came from. Reported so a stand-in cannot look like a report. */
enum class RegionSource : std::uint8_t {
    /** The client reported the region it is in. */
    reported,
    /** No report has arrived, so the destination's arrival slice set stood in. */
    arrival,
};

/** Whether one exact source binding's advertisement can be built now. */
enum class AdvertisementState : std::uint8_t {
    /** It builds, so a push carries the descriptor and the host session. */
    ready,
    /** Only the source-bound target is missing, and the next service slice fills it. */
    pending,
    /** The channel advertises nothing at all. */
    absent,
};

/**
 * Builds a citizen advertisement from one exact source activity generation.
 * A successful call retains the returned host generation until the publisher releases it with
 * `group::release_host_session`.
 * @param source Exact source record whose destination the target must copy.
 * @param regionIndex Concrete region the roster publishes for this session.
 * @param regionSource Where that index came from.
 * @param localMemberSlot Member slot of the joining client.
 * @param output Cleared, then filled when the whole channel can be advertised.
 * @param hostGeneration Cleared, then receives the retained row generation on success.
 */
void build_advertisement(
    const state::activity::SessionBinding& source,
    std::int32_t regionIndex,
    RegionSource regionSource,
    std::uint8_t localMemberSlot,
    middleware::bap::activity_message::replicate_membership::CitizenAdvertisement& output,
    std::uint64_t& hostGeneration,
    bool publicRegion = false) noexcept;

/**
 * Builds one further directory entry from the same source, without reporting an outcome.
 * Only the region a body publishes is logged, not every region the body lists.
 * @param source Exact source record whose destination the target must copy.
 * @param regionIndex Concrete region this entry names.
 * @param localMemberSlot Member slot of the joining client.
 * @param output Cleared, then filled when the whole channel can be advertised.
 * @param hostGeneration Cleared, then receives the retained row generation on success.
 */
void build_directory_entry(
    const state::activity::SessionBinding& source,
    std::int32_t regionIndex,
    std::uint8_t localMemberSlot,
    middleware::bap::activity_message::replicate_membership::CitizenAdvertisement& output,
    std::uint64_t& hostGeneration,
    bool publicRegion = false) noexcept;

/** Claims a missing host row and reports whether its advertisement is ready. */
[[nodiscard]] AdvertisementState advertisement_state(const state::activity::SessionBinding& source,
                                                     std::int32_t regionIndex,
                                                     bool publicRegion = false) noexcept;

/**
 * Claims the region's host row and completes a pending allocation at once.
 * A join answer needs the row ready in the same frame, not one service slice later.
 * @param source Exact source record whose destination the target must copy.
 * @param regionIndex Concrete region the join burst advertises.
 */
void complete_host_session(const state::activity::SessionBinding& source,
                           std::int32_t regionIndex,
                           bool publicRegion = false) noexcept;

/** Starts the private activity's stable logical Bubble Host and completes its allocation. */
[[nodiscard]] bool complete_private_host_session(const state::activity::SessionBinding& source,
                                                 std::int32_t initialRegion) noexcept;

/**
 * Claims the private activity's Bubble Host row without allocating it.
 * The allocation advances the activity state revision, which retires any plan already prepared
 * for this frame, so a staged push claims here and lets the pump service fill the row.
 */
[[nodiscard]] bool claim_private_host_session(const state::activity::SessionBinding& source,
                                              std::int32_t initialRegion) noexcept;

/** Copies the private activity's stable logical Bubble Host row. */
[[nodiscard]] bool private_host_session(const state::activity::SessionBinding& source,
                                        group::HostSessionBinding& output) noexcept;

/** Builds the private logical host's remote-member descriptor for its current region. */
void build_private_host_advertisement(
    const state::activity::SessionBinding& source,
    std::int32_t regionIndex,
    std::uint8_t localMemberSlot,
    middleware::bap::activity_message::replicate_membership::CitizenAdvertisement& output,
    std::uint64_t& hostGeneration) noexcept;

/**
 * Source-less publisher. It clears output and claims no host row, so the channel stays silent.
 * TODO: no caller yet. It is the fail-closed answer for a publisher that holds no source binding.
 */
void build_advertisement(
    std::int32_t regionIndex,
    RegionSource regionSource,
    std::uint8_t localMemberSlot,
    middleware::bap::activity_message::replicate_membership::CitizenAdvertisement& output) noexcept;

/**
 * Source-less readiness query. It reports absent and claims no host row.
 * TODO: no caller yet. It is the fail-closed answer for a caller that holds no source binding.
 */
[[nodiscard]] AdvertisementState advertisement_state(std::int32_t regionIndex) noexcept;

} // namespace sunrise::server::gameplay
