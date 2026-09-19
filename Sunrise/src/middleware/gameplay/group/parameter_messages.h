#pragma once

#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "parameter_registry.h"

namespace sunrise::middleware::gameplay::group {

/** Registry id of a parameter update, sent by the authority. */
inline constexpr std::uint8_t kParameterUpdateId = 38;
/** Registry id of a parameter request, sent by a peer. */
inline constexpr std::uint8_t kParameterRequestId = 39;
/** Declared decoded size of a parameter update. */
inline constexpr std::uint32_t kParameterUpdateSize = 44064;
/** Declared decoded size of a parameter request. */
inline constexpr std::uint32_t kParameterRequestSize = 44056;
/** The registry holds 25 parameters, so only the low 25 bitmap bits are meaningful. */
inline constexpr std::uint8_t kParameterCount = 25;
/** Every bit with a corresponding registry entry. */
inline constexpr std::uint64_t kParameterMaskBits = (std::uint64_t{1} << kParameterCount) - 1;

/**
 * Header of a parameter request. Body widths depend on the selected parameters.
 * `walk_parameter_request` locates supported bodies and reports where unknown codecs stop it.
 */
struct ParameterRequestHeader {
    std::uint64_t sessionId{};
    /** Low 25 bits name the requested parameters. */
    std::uint64_t requestedMask{};
    bool modeFlag{};
};

/**
 * Reads the header of a parameter request.
 * @param reader Reader positioned at the body.
 * @param output Receives the header fields.
 * @return True when the session id, mode flag, and bitmap were all present.
 */
[[nodiscard]] bool read_parameter_request(encoding::bits::Reader& reader,
                                          ParameterRequestHeader& output) noexcept;

/** How far the selected request bodies of one parameter request could be located. */
struct ParameterRequestWalk {
    /** Parameters whose request body was located and consumed. */
    std::uint64_t walkedMask{};
    /** Bits left in the container once the walk stopped. */
    std::uint32_t tailBits{};
    /** First selected parameter with no known request codec, or the count when there is none. */
    std::uint8_t ambiguousParameter{kParameterCount};
    /** Set when every selected body was found, so a later message in the container is findable. */
    bool complete{};
};

/**
 * Walks the request bodies that follow a parameter request header.
 * The bodies are interleaved with no per-body length, so a parameter with no known codec makes
 * every later body unfindable. The walk stops there and reports the rest as one ambiguous tail.
 * @param reader Reader positioned immediately after the request header.
 * @param requestedMask Requested bitmap, already reduced to its meaningful bits.
 * @param walk Cleared first, then filled with how far the walk got.
 * @return True when every located body was complete. False means a body ran off the container.
 */
[[nodiscard]] bool walk_parameter_request(encoding::bits::Reader& reader,
                                          std::uint64_t requestedMask,
                                          ParameterRequestWalk& walk) noexcept;

/**
 * @param mask Requested bitmap from one request header.
 * @param parameter Registry index, 0 through 24.
 * @return True when the peer asked for that parameter.
 */
[[nodiscard]] bool requests(std::uint64_t mask, std::uint8_t parameter) noexcept;

/** Parameters this host can encode a body for, as a mask over the registry indices. */
inline constexpr std::uint64_t kEncodableParameters =
    (std::uint64_t{1} << static_cast<std::uint8_t>(Parameter::activeJoinControls))
    | (std::uint64_t{1} << static_cast<std::uint8_t>(Parameter::activityHost))
    | (std::uint64_t{1} << static_cast<std::uint8_t>(Parameter::currentActivity))
    | (std::uint64_t{1} << static_cast<std::uint8_t>(Parameter::previousActivity))
    | (std::uint64_t{1} << static_cast<std::uint8_t>(Parameter::hostSelected))
    | (std::uint64_t{1} << static_cast<std::uint8_t>(Parameter::publicSessionReservations));

/**
 * Body of registry parameter 1 `active-join-controls`.
 * The peer refuses a join into a session whose copy of this parameter holds no value.
 */
struct ActiveJoinControlsParameter {
    /** Players the selected activity holds, 0 through 32. */
    std::uint8_t totalPlayerCapacity{};
    /** Free slots the user-join lane advertises, 0 through 32. */
    std::uint8_t userJoinSlots{};
    /** Free slots the party-join lane advertises, 0 through 32. */
    std::uint8_t partyJoinSlots{};
    /** Capacity left once the players already in the session are counted, 0 through 32. */
    std::uint8_t remainingPlayerCapacity{};
    /** Enables the user-join lane. The peer clears it when a policy bit is set. */
    bool userJoinEnabled{};
    /** Enables the party-join lane. */
    bool partyJoinEnabled{};
    /** Enables the remaining-capacity lane. The peer clears it when no capacity is left. */
    bool remainingJoinEnabled{};
    /** Join-policy bits, 0 through 31. Any set bit disables the user-join lane. */
    std::uint8_t joinPolicyFlags{};
    /** Join queue mode: 0 `none`, 1 `first`, 2 `accumulate`, 3 `process`. */
    std::uint8_t joinQueueMode{};
};

/**
 * Body of registry parameter 3 `activity-host`.
 * The peer builds no activity client while this parameter holds no value.
 */
struct ActivityHostParameter {
    /** Selection nonce, equal to `currentActivityNonce` when both parameters are carried. */
    std::uint64_t selectionId{};
    /** Host identity. The peer refuses the parameter while this reads zero. */
    std::uint64_t hostId{};
    /** Bit per member index. The peer needs the bit for its own member set. */
    std::uint32_t memberMask{};
    /** Host address in host order. The peer builds a plain-UDP address object from it. */
    std::uint32_t address{};
    /** Host port in host order. */
    std::uint16_t port{};
};

/**
 * Body of a parameter update.
 * A joining peer needs one applied to finish its join, whatever the update names.
 */
struct ParameterUpdate {
    std::uint64_t sessionId{};
    /** Set makes the peer rebuild its parameter object before applying the rest. */
    bool resetFlag{};
    /** Low 25 bits name parameters the peer drops. An empty slot makes that a no-op. */
    std::uint64_t releasedMask{};
    /** Low 25 bits name parameters the body carries, each one encoded after its own presence
     *  bit. Only `kEncodableParameters` may be named. */
    std::uint64_t carriedMask{};
    /** Read only when `carriedMask` names `activeJoinControls`. */
    ActiveJoinControlsParameter activeJoinControls{};
    /** Read only when `carriedMask` names `activityHost`. */
    ActivityHostParameter activityHost{};
    /** Read only when `carriedMask` names `hostSelected`. True says a host member is elected. */
    bool hostSelected{};
    /** Logical launch reason published in `currentActivity`. */
    std::int8_t currentActivityReason{-1};
    /** Logical actual-activity index published in `currentActivity`'s field at +2. */
    std::int16_t currentActualActivityIndex{-1};
    /** Logical active-activity index published in `currentActivity`. */
    std::int16_t currentActivityIndex{-1};
    /**
     * Selection nonce for `currentActivity`. A valid body requires a concrete activity index, and
     * this must equal `activityHost.selectionId` when both parameters are carried.
     */
    std::uint64_t currentActivityNonce{};
    /** The descriptor `currentActivity` replaced, published in `previousActivity`. The absent
     *  defaults write one clear root bit, which is the peer's own initialised value. */
    std::int8_t previousActivityReason{-1};
    /** See previousActivityReason. */
    std::int16_t previousActualActivityIndex{-1};
    /** See previousActivityReason. */
    std::int16_t previousActivityIndex{-1};
    /** See previousActivityReason. */
    std::uint64_t previousActivityNonce{};
};

/**
 * Writes a parameter update.
 * @param writer Writer positioned at the body.
 * @param body Update to publish.
 * @return True when it fit. False when `carriedMask` names a parameter outside
 *         `kEncodableParameters`, because the rest have no encoder here.
 */
[[nodiscard]] bool write_parameter_update(encoding::bits::Writer& writer,
                                          const ParameterUpdate& body) noexcept;

} // namespace sunrise::middleware::gameplay::group
