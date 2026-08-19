#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "command_deduplicator.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Fixed credit storage bounds participant and contribution-channel work per world. */
inline constexpr std::size_t kParticipantCreditCapacity = 16;
inline constexpr std::size_t kParticipantCreditChannelCapacity = 8;
/** The invalid participant slot lies immediately outside fixed credit storage. */
inline constexpr std::uint16_t kInvalidParticipantCreditSlot =
    static_cast<std::uint16_t>(kParticipantCreditCapacity);

struct ParticipantCreditHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{kInvalidParticipantCreditSlot};
};

enum class ParticipantCreditEventKind : std::uint8_t {
    presence,
    contribution,
};

enum class ParticipantCreditResult : std::uint8_t {
    applied,
    saturated,
    duplicate,
    invalidCommand,
    staleWorld,
    invalidHandle,
    invalidDefinition,
    invalidChannel,
    invalidAmount,
    staleTick,
    revisionExhausted,
    capacity,
};

struct ParticipantCreditDefinition final {
    std::array<std::uint32_t, kParticipantCreditChannelCapacity> channelIds{};
    std::uint64_t participantId{};
    PolicyOwnerId policyOwnerId{};
    std::size_t channelCount{};
};

struct ParticipantCreditChannel final {
    std::uint64_t value{};
    std::uint32_t channelId{};
};

struct ParticipantCreditEvent final {
    ParticipantCreditHandle participant{};
    WorldEpoch worldEpoch{};
    TickId tick{};
    std::uint64_t participantId{};
    PolicyOwnerId policyOwnerId{};
    std::uint64_t revision{};
    std::uint64_t oldValue{};
    std::uint64_t newValue{};
    std::uint32_t channelId{};
    ParticipantCreditEventKind kind{ParticipantCreditEventKind::presence};
    bool saturated{};
};

struct ParticipantCreditSnapshot final {
    ParticipantCreditDefinition definition{};
    std::array<ParticipantCreditChannel, kParticipantCreditChannelCapacity> channels{};
    std::uint64_t generation{};
    std::uint64_t revision{};
    TickId firstEligibleTick{};
    TickId lastEligibleTick{};
    std::uint64_t presenceTicks{};
    bool active{};
    bool hasPresence{};
};

struct ParticipantCreditLedgerSnapshot final {
    std::array<ParticipantCreditSnapshot, kParticipantCreditCapacity> participants{};
    CommandDeduplicatorSnapshot commands{};
    WorldEpoch worldEpoch{};
};

/** Fixed deterministic presence and contribution accounting. */
class ParticipantCreditLedger final {
public:
    /** Clears every participant and binds future commands to one world epoch. */
    void reset(WorldEpoch worldEpoch) noexcept;

    /** Creates one participant with an explicit sorted channel set. */
    [[nodiscard]] ParticipantCreditResult
    create_participant(const ParticipantCreditDefinition& definition,
                       ParticipantCreditHandle& handle) noexcept;

    /** Removes one exact participant generation. */
    [[nodiscard]] ParticipantCreditResult
    remove_participant(ParticipantCreditHandle handle) noexcept;

    /** Records one eligible fixed tick at most once for this participant. */
    [[nodiscard]] ParticipantCreditResult record_presence(const CommandHeader& command,
                                                          ParticipantCreditHandle handle,
                                                          ParticipantCreditEvent& event) noexcept;

    /** Adds one policy-defined contribution with unsigned saturation. */
    [[nodiscard]] ParticipantCreditResult
    record_contribution(const CommandHeader& command,
                        ParticipantCreditHandle handle,
                        std::uint32_t channelId,
                        std::uint64_t amount,
                        ParticipantCreditEvent& event) noexcept;

    /** Releases command ids older than the durable replay floor. */
    void expire_commands_before(TickId firstRetainedTick) noexcept;

    /** Copies one committed participant summary. */
    [[nodiscard]] bool query(ParticipantCreditHandle handle,
                             ParticipantCreditSnapshot& participant) const noexcept;

    /** Copies fixed participant and command state. */
    [[nodiscard]] ParticipantCreditLedgerSnapshot snapshot() const noexcept;

    /** Restores a validated fixed snapshot. */
    [[nodiscard]] bool restore(const ParticipantCreditLedgerSnapshot& snapshot) noexcept;

private:
    [[nodiscard]] static bool
    valid_definition(const ParticipantCreditDefinition& definition) noexcept;
    [[nodiscard]] bool valid_handle(ParticipantCreditHandle handle) const noexcept;

    std::array<ParticipantCreditSnapshot, kParticipantCreditCapacity> participants_{};
    CommandDeduplicator commands_{};
    WorldEpoch worldEpoch_{};
};

} // namespace sunrise::server::gameplay::physics::mechanics
