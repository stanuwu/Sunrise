#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "host_command.h"

namespace sunrise::server::gameplay::physics::world {

/** Admission result before any world mutation. */
enum class CommandSubmitStatus : std::uint8_t {
    accepted,
    coalesced,
    duplicate,
    invalid,
    full,
    wrongWorld,
    late,
    policyDenied,
};

/** Fixed multi-producer ingress with one deterministic consumer. */
class CommandQueue final {
public:
    CommandQueue() = default;
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    /** Clears queued commands, ordering serials, and overflow state. */
    void reset() noexcept;
    /** @return Admission result for one fully stamped command. */
    [[nodiscard]] CommandSubmitStatus push(const HostCommand& command) noexcept;
    /** @return Number of exact-tick commands copied in stable order. */
    [[nodiscard]] std::size_t drain_tick(TickId tick, std::span<HostCommand> output) noexcept;
    /** @return Current fixed queue use. */
    [[nodiscard]] std::size_t size() const noexcept;
    /** @return True after any critical command exceeds fixed storage. */
    [[nodiscard]] bool overflowed() const noexcept;

private:
    friend class WorldRunner;
    friend struct CommandQueueTestAccess;

    struct Entry final {
        HostCommand command{};
        std::uint64_t ingressSerial{};
        bool occupied{};
    };

    [[nodiscard]] bool begin_transaction() noexcept;
    void commit_transaction() noexcept;
    void rollback_transaction() noexcept;

    mutable SRWLOCK mutex_{SRWLOCK_INIT};
    std::array<Entry, kCommandQueueCapacity> entries_{};
    std::array<Entry, kCommandQueueCapacity> rollbackEntries_{};
    std::uint64_t nextIngressSerial_{1};
    std::uint64_t rollbackNextIngressSerial_{1};
    std::size_t size_{};
    std::size_t rollbackSize_{};
    bool overflowed_{};
    bool rollbackOverflowed_{};
    bool transactionActive_{};
};

} // namespace sunrise::server::gameplay::physics::world
