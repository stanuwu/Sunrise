#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../activity_host_manager/request/selection/definition.h"

namespace sunrise::middleware::bap::activity_message::start_activity {

/** The client asks to start a different activity from inside the world. */
inline constexpr std::uint32_t kMessageType = 11;

/** The bounded selector opens the body. */
inline constexpr std::uint8_t kSelectorWidth = 4;
/** Both activity indices are twelve bits wide. */
inline constexpr std::uint8_t kActivityIndexWidth = 12;
/** The optional activity element is nine bits wide. */
inline constexpr std::uint8_t kElementWidth = 9;
/** The two optional identity words are full width. */
inline constexpr std::uint8_t kIdentityWidth = 64;
/** Every optional field is introduced by one presence bit. */
inline constexpr std::uint8_t kPresenceWidth = 1;
/** The selector, both indices, and the optional element all carry a bias of one. */
inline constexpr std::int32_t kIndexBias = 1;
/** A biased field that decodes to this value names nothing. */
inline constexpr std::int32_t kAbsentIndex = -1;

/**
 * The part of a start-new-activity request whose grammar is recovered.
 * The body continues with a nested selection record of unresolved shape, so everything from that
 * record on is retained as one bounded tail instead of being walked.
 */
struct StartActivity {
    /** Bounded selector. Its value meanings are not recovered. */
    std::int32_t selector{};
    /** Activity index the client is leaving. */
    std::int32_t sourceActivityIndex{};
    /** Activity index the client is asking for. */
    std::int32_t destinationActivityIndex{};
    /** Optional activity element, or the absent index when the body carried none. */
    std::int32_t element{kAbsentIndex};
    /** Optional opaque identity. */
    std::int64_t identity{};
    /** Optional account or selection identity, read as unsigned. */
    std::uint64_t selectionIdentity{};
    /** Bits left after the last recovered field. Their grammar is unresolved. */
    std::uint32_t tailBits{};
    /** Read-only decode of the descriptor continuation that follows the identity prefix. */
    activity_host_manager::request::selection::ActivityManagerSelection continuation{};
    bool hasElement{};
    bool hasIdentity{};
    bool hasSelectionIdentity{};
    /** True when the complete continuation decoded without changing framing policy. */
    bool hasContinuation{};
};

/**
 * Parses a start-new-activity request as far as its recovered grammar reaches.
 * @param input Activity payload after the envelope.
 * @param request Cleared first, then filled with every field that was reached.
 * @param consumedBits Receives the bits the recovered fields used.
 * @return True when every recovered field was present.
 */
[[nodiscard]] bool parse_start_activity(std::span<const std::byte> input,
                                        StartActivity& request,
                                        std::size_t& consumedBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::start_activity
