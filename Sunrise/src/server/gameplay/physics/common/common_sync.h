#pragma once

#include <array>
#include <cstdint>

#include "../../../../middleware/gameplay/external/common_state.h"

namespace sunrise::server::gameplay::physics::common {

/** Exact ActivityClient lifetime bound to one common-generation exchange. */
struct Binding final {
    std::array<std::uint64_t, 2> patchEpoch{};
    std::uint64_t activitySessionId{};
    std::uint64_t bindingGeneration{};
};

/** Common synchronization phase for one ActivityClient lifetime. */
enum class Phase : std::uint8_t {
    absent,
    awaitingObservation,
    observed,
    advanceQueued,
    confirmed,
};

/** Result of one inbound common-root observation. */
enum class ObserveResult : std::uint8_t {
    initial,
    unchanged,
    confirmed,
    malformed,
    wrongBinding,
    unexpectedGeneration,
};

/** Transactional activity-message-44 plan. */
struct AdvancePlan final {
    Binding binding{};
    std::uint64_t stateGeneration{};
    std::uint8_t previousGeneration{};
    std::uint8_t nextGeneration{};
    bool prepared{};
};

/** Immutable confirmed common root prepared for one packet transaction. */
struct CommonPlan final {
    middleware::gameplay::external::CommonState common{};
    std::uint64_t stateGeneration{};
    std::uint8_t reconciliationGeneration{};
    bool prepared{};
};

/** Exact handle to one pending common-root packet outcome. */
struct CommonContributionHandle final {
    std::uint64_t stateGeneration{};
    std::uint64_t generation{};
    std::uint64_t packetGeneration{};
};

enum class CommonOutcome : std::uint8_t {
    success,
    lost,
};

/** Per-ActivityClient common synchronization state. */
struct State final {
    Binding binding{};
    std::uint64_t generation{};
    std::uint64_t nextContributionGeneration{1};
    std::uint64_t pendingContributionGeneration{};
    std::uint64_t pendingPacketGeneration{};
    std::uint8_t observedGeneration{};
    std::uint8_t targetGeneration{};
    Phase phase{Phase::absent};
    bool commonPending{};
    bool commonCovered{};
};

/** Clears a common synchronization lifetime. */
void reset(State& state) noexcept;

/** Binds a fresh ActivityClient lifetime and waits for its first common root. */
[[nodiscard]] bool bind(State& state, const Binding& binding) noexcept;

/** Applies one exact inbound common root without accepting a different activity. */
[[nodiscard]] ObserveResult
observe(State& state, const middleware::gameplay::external::CommonState& common) noexcept;

/** Prepares the only allowed next-generation transition without changing state. */
[[nodiscard]] bool prepare_advance(const State& state, AdvancePlan& plan) noexcept;

/** Marks a prepared activity message 44 as queued for the bound ActivityClient. */
[[nodiscard]] bool commit_advance(State& state, const AdvancePlan& plan) noexcept;

/** Builds the confirmed count-one common root for an external contribution. */
[[nodiscard]] bool confirmed_common(const State& state,
                                    middleware::gameplay::external::CommonState& common) noexcept;

/** Prepares a confirmed common root without changing outcome state. */
[[nodiscard]] bool prepare_common(const State& state, CommonPlan& plan) noexcept;

/** Attaches one prepared common root to an eligible packet generation. */
[[nodiscard]] bool commit_common(State& state,
                                 const CommonPlan& plan,
                                 std::uint64_t packetGeneration,
                                 CommonContributionHandle& handle) noexcept;

/** Settles one exact common-root packet contribution once. */
[[nodiscard]] bool settle_common(State& state,
                                 CommonContributionHandle handle,
                                 std::uint64_t packetGeneration,
                                 CommonOutcome outcome) noexcept;

/** Reports whether an eligible packet outcome covered the common baseline. */
[[nodiscard]] bool common_covered(const State& state) noexcept;

/** Reports whether a later common root confirmed the queued generation. */
[[nodiscard]] bool ready(const State& state) noexcept;

} // namespace sunrise::server::gameplay::physics::common
