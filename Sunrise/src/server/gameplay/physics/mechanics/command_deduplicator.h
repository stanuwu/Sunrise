#pragma once

#include <array>
#include <cstddef>

#include "mechanics_types.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Each mechanics service retains 256 command identities until the durable floor advances. */
inline constexpr std::size_t kCommandDeduplicationCapacity = 256;

enum class DeduplicationResult : std::uint8_t {
    recorded,
    duplicate,
    invalid,
    capacity,
};

struct CommandRecord final {
    CommandId commandId{};
    TickId executionTick{};
    PolicyOwnerId policyOwnerId{};
    bool occupied{};
};

struct CommandDeduplicatorSnapshot final {
    std::array<CommandRecord, kCommandDeduplicationCapacity> records{};
    std::size_t count{};
};

/** Fixed command history used after a command passes all semantic checks. */
class CommandDeduplicator final {
public:
    /** Clears every retained command id. */
    void clear() noexcept;

    /** @return True when the policy owner already used this command id. */
    [[nodiscard]] bool contains(PolicyOwnerId policyOwnerId, CommandId commandId) const noexcept;

    /** Records one accepted command without evicting a live id. */
    [[nodiscard]] DeduplicationResult remember(const CommandHeader& header) noexcept;

    /** Removes records older than the caller's durable replay floor. */
    void expire_before(TickId firstRetainedTick) noexcept;

    /** @return The number of retained command ids. */
    [[nodiscard]] std::size_t size() const noexcept;

    /** Copies fixed state for deterministic checkpoints. */
    [[nodiscard]] CommandDeduplicatorSnapshot snapshot() const noexcept;

    /** Replaces fixed state after validating count, ids, and uniqueness. */
    [[nodiscard]] bool restore(const CommandDeduplicatorSnapshot& snapshot) noexcept;

private:
    std::array<CommandRecord, kCommandDeduplicationCapacity> records_{};
    std::size_t count_{};
};

} // namespace sunrise::server::gameplay::physics::mechanics
