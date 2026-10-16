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
namespace unlocks = state::unlocks;

/** The native slot maps mark unmapped positions with the all-ones row value. */
constexpr std::uint16_t kUnmappedRow = 0xFFFFU;
/** Native class of an eight-byte unlock instruction in an interaction or sale gate. */
constexpr std::uint32_t kInstructionClass = 0x80807D31U;
/** Native class of a sale gate list's sixteen-byte expression descriptors. */
constexpr std::uint32_t kProgramListClass = 0x80807D2FU;

/** One unbound opcode and native slot or literal read from a vendor row. */
struct NativeInstruction {
    unlocks::Opcode opcode{};
    std::int32_t operand{};
};

/** Native instruction storage before unlock slots are mapped to saved banks. */
struct NativeProgram {
    std::array<NativeInstruction, domain::kVendorProgramCapacity> instructions{};
    std::size_t count{};
};

/**
 * Appends one native instruction array, refusing unknown opcodes and capacity overflow.
 * @param blob Vendor definition bytes.
 * @param field Array descriptor offset.
 * @param program Accumulates the decoded instructions.
 * @return False when the array cannot be decoded within the fixed capacity.
 */
[[nodiscard]] bool append_program(std::span<const std::byte> blob,
                                  std::size_t field,
                                  NativeProgram& program) noexcept {
    tables::Array rows{};
    if (!tables::read_array(blob, field, kInstructionClass, tables::kUnlockInstructionStride, rows)
        || rows.count > program.instructions.size() - program.count) {
        return false;
    }
    for (std::size_t index = 0; index < rows.count; ++index) {
        const auto at = rows.dataOffset + index * tables::kUnlockInstructionStride;
        std::uint32_t nativeOpcode = 0;
        std::int32_t operand = 0;
        unlocks::Opcode opcode{};
        if (!tables::read(blob, at, nativeOpcode)
            || !tables::read(blob, at + tables::kUnlockInstructionOperandOffset, operand)
            || !unlocks::decode_opcode(nativeOpcode, opcode)) {
            return false;
        }
        program.instructions[program.count++] = {opcode, operand};
    }
    return true;
}

/** Reads one interaction's direct native instruction array. */
[[nodiscard]] bool
read_program(std::span<const std::byte> blob, std::size_t field, NativeProgram& output) noexcept {
    output = {};
    return append_program(blob, field, output);
}

/**
 * Joins a sale's authored program list with postfix AND.
 * @param blob Vendor definition bytes.
 * @param field Program-list descriptor offset.
 * @param output Receives the complete program, or remains empty on failure.
 * @return False when a listed program is empty, malformed or over capacity.
 */
[[nodiscard]] bool read_program_list(std::span<const std::byte> blob,
                                     std::size_t field,
                                     NativeProgram& output) noexcept {
    output = {};
    tables::Array rows{};
    if (!tables::read_array(
            blob, field, kProgramListClass, tables::kUnlockExpressionFieldSize, rows)) {
        return false;
    }
    NativeProgram parsed{};
    for (std::size_t index = 0; index < rows.count; ++index) {
        const auto at = rows.dataOffset + index * tables::kUnlockExpressionFieldSize;
        const auto before = parsed.count;
        if (!append_program(blob, at, parsed) || parsed.count == before) {
            return false;
        }
        if (index != 0) {
            if (parsed.count == parsed.instructions.size()) {
                return false;
            }
            parsed.instructions[parsed.count++] = {unlocks::Opcode::logicalAnd, 0};
        }
    }
    output = parsed;
    return true;
}

/** Binds one input to at most one saved bank; unmapped slots require Family-5 values. */
[[nodiscard]] bool bind_input(const NativeInstruction& instruction,
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

/**
 * Retains native slots for callback lookup and maps only inputs the expression reads.
 * @param program Decoded native instructions.
 * @param maps Installed unlock-slot mappings.
 * @param gate Receives the bound program and inputs.
 * @return False when an input is ambiguous, out of range or over capacity.
 */
[[nodiscard]] bool
bind_gate(const NativeProgram& program, const GateMaps& maps, domain::Gate& gate) noexcept {
    gate = {};
    gate.program.count = program.count;
    for (std::size_t row = 0; row < program.count; ++row) {
        const auto& instruction = program.instructions[row];
        const bool readsSlot = instruction.opcode == domain::Opcode::flag
                               || instruction.opcode == domain::Opcode::loadValue;
        auto& bound = gate.program.instructions[row];
        // Keep the native slot so a Family-5 override can precede its saved-bank mapping.
        bound = {instruction.opcode,
                 readsSlot ? unlocks::Bank::external : unlocks::Bank::none,
                 static_cast<std::uint32_t>(instruction.operand)};
        if (!unlocks::valid(bound)) {
            return false;
        }
        if (!readsSlot) {
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
    return sale.categoryIndex != domain::kAbsentCategoryIndex
           && sale.priceState == domain::PriceState::plain && sale.costCount == 0
           && state::build_data::find_item_definition_index(sale.itemIndex, item)
           && state::build_data::find_reward_item(sale.itemIndex, reward)
           && item.definitionHash == reward.definitionHash
           && reward.poolIndex != state::build_data::rewards::kAbsent
           && (reward.flags & state::build_data::rewards::kOpenOnAcquisition) != 0;
}

/**
 * Appends one vendor's package-sale, interaction and reply gates only after complete decoding.
 * @param source Borrowed package source; no package spans escape this call.
 * @param scratch Reusable package-reader storage.
 * @param maps Borrowed installed unlock-slot mappings.
 * @param vendor Published vendor identity and expected array bounds.
 * @param interactions Receives copied interaction and reply gates on success.
 * @param sales Receives copied sale gates on success.
 * @return False for unreadable or unsupported data; neither output is appended on failure.
 */
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
        NativeProgram admission{}, selection{};
        domain::SaleGates gates{};
        if (!domain::sale_row(vendor, row, sale)
            || !read_program_list(bytes, at + kSaleAdmissionField, admission)
            || !read_program_list(bytes, at + kSaleSelectionField, selection)
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
        NativeProgram condition{};
        domain::InteractionGate gate{};
        tables::Array replies{};
        if (!read_program(bytes, at + kInteractionConditionField, condition)
            || !bind_gate(condition, maps, gate.condition)
            || !tables::read_array(
                bytes, at + kInteractionReplyField, kReplyRowClass, kReplyRowStride, replies)) {
            return false;
        }
        for (std::size_t reply = 0; reply < replies.count; ++reply) {
            NativeProgram program{};
            domain::Gate replyCondition{};
            if (!read_program(bytes, replies.dataOffset + reply * kReplyRowStride, program)
                || !bind_gate(program, maps, replyCondition)) {
                return false;
            }
            gate.replyConditions.push_back(replyCondition);
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
