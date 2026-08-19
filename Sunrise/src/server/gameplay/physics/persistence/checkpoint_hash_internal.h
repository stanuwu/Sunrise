#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "checkpoint.h"

namespace sunrise::server::gameplay::physics::persistence::detail {

/** Architecture-independent little-endian FNV-1a checkpoint writer. */
class CheckpointHashWriter final {
public:
    /** Appends one integral or enum value without native padding. */
    template <typename Value> void scalar(Value value) noexcept {
        static_assert(std::is_integral_v<Value> || std::is_enum_v<Value>);
        if constexpr (std::is_same_v<std::remove_cv_t<Value>, bool>) { // Bools hash as one byte.
            append_unsigned(static_cast<std::uint8_t>(value ? 1U : 0U));
        } else if constexpr (std::is_enum_v<Value>) { // Enums hash through their declared base.
            append_unsigned(
                static_cast<std::make_unsigned_t<std::underlying_type_t<Value>>>(value));
        } else {
            append_unsigned(static_cast<std::make_unsigned_t<Value>>(value));
        }
    }

    void bytes(std::span<const std::byte> values) noexcept;
    [[nodiscard]] std::uint64_t finish() const noexcept;

private:
    template <typename Value> void append_unsigned(Value value) noexcept {
        for (std::size_t index = 0; index < sizeof value; ++index) {
            hash_ ^= static_cast<std::uint8_t>(value & 0xFFU);
            hash_ *= 1'099'511'628'211ULL;
            value >>= 8U;
        }
    }

    std::uint64_t hash_{14'695'981'039'346'656'037ULL};
};

void hash_timers(CheckpointHashWriter& hash,
                 const mechanics::TickTimerServiceSnapshot& snapshot) noexcept;
void hash_triggers(CheckpointHashWriter& hash,
                   const mechanics::TriggerSystemSnapshot& snapshot) noexcept;
void hash_objectives(CheckpointHashWriter& hash,
                     const mechanics::ObjectiveLedgerSnapshot& snapshot) noexcept;
void hash_credit(CheckpointHashWriter& hash,
                 const mechanics::ParticipantCreditLedgerSnapshot& snapshot) noexcept;
void hash_combat(CheckpointHashWriter& hash,
                 const mechanics::CombatKernelSnapshot& snapshot) noexcept;

} // namespace sunrise::server::gameplay::physics::persistence::detail
