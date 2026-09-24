#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../middleware/content/packages/tables/field_reader.h"
#include "../../../state/build_data/rewards/reward_catalog.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/vendors/vendor_catalog.h"
#include "../../../state/build_data/vendors/vendor_gate_catalog.h"
#include "layout.h"
#include "vendor_build.h"

namespace sunrise::client::content::vendors {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::vendors;

/** The native slot maps mark unmapped positions with the all-ones row value. */
constexpr std::uint16_t kUnmappedRow = 0xFFFFU;

/** Binds one input to at most one saved bank; unmapped slots require Family-5 values. */
[[nodiscard]] bool bind_input(const domain::Instruction& instruction,
                              const GateMaps& maps,
                              domain::GateInput& output) noexcept {
    output = {};
    const auto slot = instruction.operand;
    if (slot < 0 || static_cast<std::size_t>(slot) >= maps.accountFlag.size()
        || static_cast<std::size_t>(slot) >= maps.profileFlag.size()
        || static_cast<std::size_t>(slot) >= maps.characterFlag.size()
        || static_cast<std::size_t>(slot) >= maps.accountValue.size()
        || static_cast<std::size_t>(slot) >= maps.characterValue.size()) {
        return false;
    }
    output.opcode = instruction.opcode;
    output.slot = static_cast<std::uint16_t>(slot);
    output.bank = domain::GateBank::external;
    const auto select = [&](std::span<const std::uint16_t> map, domain::GateBank bank) {
        if (map[slot] == kUnmappedRow) {
            return true;
        }
        if (output.bank != domain::GateBank::external) {
            return false;
        }
        output.bank = bank;
        output.row = map[slot];
        return true;
    };
    if (instruction.opcode == domain::Opcode::flag) {
        return select(maps.accountFlag, domain::GateBank::accountFlag)
               && select(maps.profileFlag, domain::GateBank::profileFlag)
               && select(maps.characterFlag, domain::GateBank::characterFlag);
    }
    return select(maps.accountValue, domain::GateBank::accountValue)
           && select(maps.characterValue, domain::GateBank::characterValue);
}

/** Resolves only the input instructions an expression actually reads. */
[[nodiscard]] bool
bind_gate(const domain::Program& program, const GateMaps& maps, domain::Gate& gate) noexcept {
    gate = {};
    gate.program = program;
    for (std::size_t row = 0; row < program.count; ++row) {
        const auto& instruction = program.instructions[row];
        if (instruction.opcode != domain::Opcode::flag
            && instruction.opcode != domain::Opcode::loadValue) {
            continue;
        }
        const auto inputs = std::span{gate.inputs}.first(gate.inputCount);
        const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto& input) {
            return input.opcode == instruction.opcode && input.slot == instruction.operand;
        });
        if (found != inputs.end()) {
            continue;
        }
        if (gate.inputCount == gate.inputs.size()
            || !bind_input(instruction, maps, gate.inputs[gate.inputCount])) {
            return false;
        }
        ++gate.inputCount;
    }
    return true;
}

/** A free sale is a candidate only when its installed wrapper opens a reward pool. */
[[nodiscard]] bool reward_package(const domain::SaleRow& sale) noexcept {
    state::build_data::items::Definition item{};
    state::build_data::rewards::Item reward{};
    return sale.categoryIndex != domain::kAbsentCategoryIndex && sale.costQuantity == 0
           && sale.costItemIndex == domain::kAbsentCostItem
           && state::build_data::find_item_definition_index(sale.itemIndex, item)
           && state::build_data::rewards::find_item(sale.itemIndex, reward)
           && item.definitionHash == reward.definitionHash
           && reward.poolIndex != state::build_data::rewards::kAbsent
           && (reward.flags & state::build_data::rewards::kOpenOnAcquisition) != 0;
}

/** Reads one vendor's relevant sale and matching interaction gates as one unit. */
[[nodiscard]] bool read_vendor(const reader::Source& source,
                               reader::Scratch& scratch,
                               const GateMaps& maps,
                               const domain::Definition& vendor,
                               std::vector<domain::InteractionGate>& interactions,
                               std::vector<domain::SaleGates>& sales) noexcept {
    std::vector<std::uint16_t> candidates;
    for (std::size_t row = 0; row < vendor.saleCount; ++row) {
        domain::SaleRow sale{};
        if (domain::sale_row(vendor, row, sale) && reward_package(sale)) {
            candidates.push_back(static_cast<std::uint16_t>(row));
        }
    }
    if (candidates.empty()) {
        return true;
    }
    std::vector<std::byte> blob;
    std::uint32_t classId = 0;
    if (!reader::read_tag(source, scratch, vendor.definitionTag, blob, classId)
        || classId != domain::kDefinitionClass || blob.size() != vendor.definitionSize) {
        return false;
    }
    const std::span<const std::byte> bytes{blob};
    tables::Array saleRows{}, interactionRows{};
    if (!tables::read_array(
            bytes, kSaleArrayDescriptor, domain::kSaleRowClass, domain::kSaleRowStride, saleRows)
        || saleRows.count != vendor.saleCount || saleRows.dataOffset != vendor.saleRowBase
        || !tables::read_array(bytes,
                               kThirdArrayDescriptor,
                               kInteractionRowClass,
                               domain::kThirdRowStride,
                               interactionRows)
        || interactionRows.count != vendor.thirdCount
        || interactionRows.dataOffset != vendor.thirdRowBase) {
        return false;
    }
    std::vector<domain::SaleGates> ownSales;
    for (const auto row : candidates) {
        const auto at = saleRows.dataOffset + row * domain::kSaleRowStride;
        domain::SaleRow sale{};
        domain::Program admission{}, selection{};
        domain::SaleGates gates{};
        if (!domain::sale_row(vendor, row, sale)
            || !domain::read_program_list(bytes, at + kSaleAdmissionField, admission)
            || !domain::read_program_list(bytes, at + kSaleSelectionField, selection)
            || !bind_gate(admission, maps, gates.admission)
            || !bind_gate(selection, maps, gates.selection)) {
            return false;
        }
        gates.vendorHash = vendor.definitionHash;
        gates.index = row;
        gates.categoryIndex = sale.categoryIndex;
        ownSales.push_back(gates);
    }
    std::vector<domain::InteractionGate> ownInteractions;
    for (std::size_t row = 0; row < interactionRows.count; ++row) {
        const auto at = interactionRows.dataOffset + row * domain::kThirdRowStride;
        std::int32_t category = 0;
        if (!tables::read(bytes, at + kInteractionCategoryOffset, category)) {
            return false;
        }
        if (std::none_of(ownSales.begin(), ownSales.end(), [&](const auto& sale) {
                return sale.categoryIndex == category;
            })) {
            continue;
        }
        domain::Program condition{};
        domain::InteractionGate gate{};
        if (!domain::read_program(bytes, at + kInteractionConditionField, condition)
            || !bind_gate(condition, maps, gate.condition)) {
            return false;
        }
        gate.vendorHash = vendor.definitionHash;
        gate.index = static_cast<std::uint16_t>(row);
        gate.categoryIndex = category;
        ownInteractions.push_back(gate);
    }
    sales.insert(sales.end(), ownSales.begin(), ownSales.end());
    interactions.insert(interactions.end(), ownInteractions.begin(), ownInteractions.end());
    return true;
}

} // namespace

/** Reads only faction vendors with free installed reward wrappers. */
bool build_gates(const reader::Source& source,
                 reader::Scratch& scratch,
                 const GateMaps& maps) noexcept {
    if (domain::gates_ready()) {
        return true;
    }
    static std::array<domain::Definition, domain::kDefinitionCapacity> definitions{};
    std::size_t count = 0;
    if (!domain::snapshot_definitions(definitions, count)) {
        return false;
    }
    std::vector<domain::InteractionGate> interactions;
    std::vector<domain::SaleGates> sales;
    std::size_t skipped = 0;
    for (std::size_t row = 0; row < count; ++row) {
        const auto& vendor = definitions[row];
        if (vendor.factionProgressionIndex == domain::kUnavailableFactionProgressionIndex) {
            continue;
        }
        if (!read_vendor(source, scratch, maps, vendor, interactions, sales)) {
            ++skipped;
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=build_data stage=vendor_gates result=skip hash=0x%08X",
                              vendor.definitionHash);
        }
    }
    const bool published = domain::replace_gates(interactions, sales);
    core::log::writef(
        core::log::Channel::state,
        published ? core::log::Level::info : core::log::Level::warn,
        "ev=build_data stage=vendor_gates result=%s interactions=%zu sales=%zu skipped=%zu",
        published ? "ok" : "fail",
        interactions.size(),
        sales.size(),
        skipped);
    return published;
}

} // namespace sunrise::client::content::vendors
