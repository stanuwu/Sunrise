#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "vendor_expression.h"

namespace sunrise::state::build_data::vendors {

/** A Family-5 override takes precedence; otherwise a gate reads its mapped saved bank. */
enum class GateBank : std::uint8_t {
    external,
    accountFlag,
    profileFlag,
    characterFlag,
    accountValue,
    characterValue,
};

/** One native slot resolved against the installed build's unlock mapping tables. */
struct GateInput {
    Opcode opcode{};
    std::uint16_t slot{};
    GateBank bank{};
    std::uint16_t row{};
};

/** One interaction or sale expression with its bounded input bindings. */
struct Gate {
    Program program{};
    std::array<GateInput, kVendorProgramCapacity> inputs{};
    std::size_t inputCount{};
};

/** Native interaction row belonging to a vendor category. */
struct InteractionGate {
    std::uint32_t vendorHash{};
    std::uint16_t index{};
    std::int32_t categoryIndex{};
    Gate condition{};
};

/** Native sale condition fields for one installed package offer. */
struct SaleGates {
    std::uint32_t vendorHash{};
    std::uint16_t index{};
    std::int32_t categoryIndex{};
    Gate admission{};
    Gate selection{};
};

/**
 * Publishes one complete process-local extraction. Cached vendor rows do not store expressions.
 * @param interactions Native interaction gates, unique by vendor and row index.
 * @param sales Native sale gates, unique by vendor and row index.
 * @return False when ranges or identities are invalid; no partial publication occurs.
 */
[[nodiscard]] bool replace_gates(std::span<const InteractionGate> interactions,
                                 std::span<const SaleGates> sales) noexcept;
/** Withdraws expressions before vendor rows are replaced or cleared. */
void clear_gates() noexcept;
/** @return True when one complete extraction was published this process. */
[[nodiscard]] bool gates_ready() noexcept;
/** @return True when the exact vendor and interaction row was retained. */
[[nodiscard]] bool find_interaction_gate(std::uint32_t vendorHash,
                                         std::uint16_t index,
                                         InteractionGate& output) noexcept;
/** @return True when the exact vendor and sale row was retained. */
[[nodiscard]] bool
find_sale_gates(std::uint32_t vendorHash, std::uint16_t index, SaleGates& output) noexcept;

} // namespace sunrise::state::build_data::vendors
