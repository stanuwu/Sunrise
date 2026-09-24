#include "vendor_gate_catalog.h"

#include <algorithm>
#include <shared_mutex>
#include <vector>

#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::vendors {
namespace {

core::threading::SrwLock g_lock;
std::vector<InteractionGate> g_interactions;
std::vector<SaleGates> g_sales;
bool g_ready{};

/** A reader must never span past one published gate's fixed storage. */
[[nodiscard]] bool valid_gate(const Gate& gate) noexcept {
    return gate.program.count <= gate.program.instructions.size()
           && gate.inputCount <= gate.inputs.size() && gate.inputCount <= gate.program.count;
}

/** @tparam Row Vendor-keyed gate row. @param rows Candidate bank. @return False for a duplicate. */
template <typename Row> [[nodiscard]] bool unique_rows(std::span<const Row> rows) noexcept {
    for (std::size_t first = 0; first < rows.size(); ++first) {
        for (std::size_t other = first + 1; other < rows.size(); ++other) {
            if (rows[first].vendorHash == rows[other].vendorHash
                && rows[first].index == rows[other].index) {
                return false;
            }
        }
    }
    return true;
}

/**
 * @tparam Row Vendor-keyed gate row.
 * @param rows Published bank.
 * @param vendorHash Vendor definition identity.
 * @param index Native row index.
 * @param output Receives the row, cleared when absent.
 * @return True when one exact row was retained.
 */
template <typename Row>
[[nodiscard]] bool find_row(std::span<const Row> rows,
                            std::uint32_t vendorHash,
                            std::uint16_t index,
                            Row& output) noexcept {
    output = {};
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const Row& row) {
        return row.vendorHash == vendorHash && row.index == index;
    });
    if (found == rows.end()) {
        return false;
    }
    output = *found;
    return true;
}

} // namespace

/** Replaces the checked process-local gate set under one lock. */
bool replace_gates(std::span<const InteractionGate> interactions,
                   std::span<const SaleGates> sales) noexcept {
    if (!unique_rows(interactions) || !unique_rows(sales)) {
        return false;
    }
    if (std::any_of(interactions.begin(),
                    interactions.end(),
                    [](const auto& row) { return !valid_gate(row.condition); })
        || std::any_of(sales.begin(), sales.end(), [](const auto& row) {
               return !valid_gate(row.admission) || !valid_gate(row.selection);
           })) {
        return false;
    }
    std::vector<InteractionGate> nextInteractions(interactions.begin(), interactions.end());
    std::vector<SaleGates> nextSales(sales.begin(), sales.end());
    const std::lock_guard guard(g_lock);
    g_interactions.swap(nextInteractions);
    g_sales.swap(nextSales);
    g_ready = true;
    return true;
}

/** Invalidates only derived gate expressions, leaving cached vendor rows intact. */
void clear_gates() noexcept {
    const std::lock_guard guard(g_lock);
    g_interactions.clear();
    g_sales.clear();
    g_ready = false;
}

/** @return True when one complete gate extraction was published this process. */
bool gates_ready() noexcept {
    const std::shared_lock guard(g_lock);
    return g_ready;
}

/** Copies one interaction gate without exposing the mutable published bank. */
bool find_interaction_gate(std::uint32_t vendorHash,
                           std::uint16_t index,
                           InteractionGate& output) noexcept {
    const std::shared_lock guard(g_lock);
    return g_ready
           && find_row(std::span<const InteractionGate>{g_interactions}, vendorHash, index, output);
}

/** Copies one sale's gates without exposing the mutable published bank. */
bool find_sale_gates(std::uint32_t vendorHash, std::uint16_t index, SaleGates& output) noexcept {
    const std::shared_lock guard(g_lock);
    return g_ready && find_row(std::span<const SaleGates>{g_sales}, vendorHash, index, output);
}

} // namespace sunrise::state::build_data::vendors
