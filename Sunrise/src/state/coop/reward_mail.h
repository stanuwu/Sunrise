#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/bap/activity_message/incident.h"
#include "../activity/destination/definition.h"

namespace sunrise::state::coop::reward_mail {

/** Sunrise-only notification that carries one reward event to the install that owns it. */
inline constexpr std::uint16_t kService = 0x8004;

/** What one event asks the owning install to credit. */
enum class Kind : std::uint8_t {
    /** A world interaction; the grant rules read its targets. */
    interaction,
    /** A kill; the bounty rules read its payload and area. */
    kill,
};

/** One reported incident, with what the grant or bounty rules read from it. */
struct Event final {
    Kind kind{Kind::interaction};
    std::uint32_t primaryTarget{};
    std::array<std::uint32_t, middleware::bap::activity_message::incident::kExtraTargetMaximum>
        extraTargets{};
    std::uint32_t extraTargetCount{};
    std::uint32_t payloadLength{};
    /** The incident payload; filled for a kill only. */
    std::array<std::byte, middleware::bap::activity_message::incident::kPayloadMaximum> payload{};
    /** A kill's bubble, which only the host's generated world can name. */
    bool hasArea{};
    std::uint32_t scenarioTag{};
    std::uint32_t bubbleNameHash{};
    /** The destination the reporting player was in. */
    std::array<char, activity::destination::kPackageNameCapacity> package{};
    std::uint8_t packageLength{};
};

/** Bytes of one encoded event. */
inline constexpr std::size_t kEventSize =
    1 + 1 + 4 + middleware::bap::activity_message::incident::kExtraTargetMaximum * 4 + 4 + 4
    + middleware::bap::activity_message::incident::kPayloadMaximum + 1 + 4 + 4 + 1
    + activity::destination::kPackageNameCapacity;

/** Writes one event. @return False when `output` is too small or the event is out of range. */
[[nodiscard]] bool
encode(const Event& event, std::span<std::byte> output, std::size_t& written) noexcept;
/** @return True when `input` is exactly one valid event. */
[[nodiscard]] bool decode(std::span<const std::byte> input, Event& output) noexcept;

/** Queues an event for the guest whose account key is `primarySoid`. */
void post(std::uint64_t primarySoid, const Event& event) noexcept;
/** Copies the oldest event queued for `primarySoid`. @return False when none is queued. */
[[nodiscard]] bool front(std::uint64_t primarySoid, Event& output) noexcept;
/** Drops the oldest event queued for `primarySoid`, once its frame reached the transport. */
void pop(std::uint64_t primarySoid) noexcept;

} // namespace sunrise::state::coop::reward_mail
