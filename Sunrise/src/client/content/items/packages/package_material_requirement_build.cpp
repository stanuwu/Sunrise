#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/material_requirements/material_requirement_catalog.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/** One requirement array field: its count, then its self-relative offset. */
constexpr std::size_t kRequirementArrayBytes =
    sizeof(generated::TagArray<generated::MaterialRequirementRow>);

/**
 * Reports the requirement-set extraction result once per build.
 * @param sets Sets read from the table.
 * @param conditionalRows Rows carrying a condition, counted for the same line.
 * @param valid True when the table passed its checks.
 * @param published True when the table reached the catalog, which sets the level.
 */
void report_material_requirements(std::size_t sets,
                                  std::size_t conditionalRows,
                                  bool valid,
                                  bool published) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=materials result=%s sets=%zu conditional=%zu "
                                      "valid=%u",
                                      published ? "ok" : "fail",
                                      sets,
                                      conditionalRows,
                                      static_cast<unsigned>(valid));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         published ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Names the row that stopped the requirement-set build.
 * @param reason Short key naming the failing step.
 * @param set Requirement set being read, or zero before one was picked.
 * @param row Requirement row being read, or zero before one was picked.
 */
void report_material_requirement_failure(const char* reason,
                                         std::size_t set,
                                         std::size_t row) noexcept {
    std::array<char, 144> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=pkg stage=materials result=fail reason=%s set=%zu row=%zu",
                      reason,
                      set,
                      row);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/**
 * Extracts the material requirement sets from the installed packages and publishes them.
 * The build is skipped when the catalog already holds the domain.
 * @param source Borrowed package reader.
 * @param storage Caller-owned scratch and definition buffers.
 * @param root Borrowed root table bytes.
 * @param itemDefinitionCount Item count the requirement rows are checked against.
 * @return True when the domain is ready, either already or after this build.
 */
bool build_material_requirements(const reader::Source& source,
                                 Storage& storage,
                                 std::span<const std::byte> root,
                                 std::uint64_t itemDefinitionCount) noexcept {
    namespace domain = state::build_data::material_requirements;
    if (state::build_data::material_requirement_sets_ready()) {
        return true;
    }
    if (itemDefinitionCount == 0
        || itemDefinitionCount > state::build_data::items::kDefinitionCapacity) {
        report_material_requirement_failure("item_count", 0, 0);
        return false;
    }
    std::uint32_t tableTag = 0;
    std::uint32_t tableClass = 0;
    tables::Array sets{};
    if (!tables::slot_tag(root, tables::kMaterialRequirementTableSlot, tableTag) || tableTag == 0
        || tables::package_of(tableTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, storage.scratch, tableTag, storage.definition, tableClass)
        || tableClass != tables::kMaterialRequirementTableClass
        || !tables::find_array_at(std::span<const std::byte>{storage.definition},
                                  offsetof(generated::MaterialRequirementTable, table),
                                  sets)
        || sets.elementClass != tables::kMaterialRequirementSetRowClass || sets.count == 0
        || sets.count > storage.materialRequirementRows.size()) {
        report_material_requirement_failure("table", 0, 0);
        return false;
    }
    const std::span<const std::byte> table{storage.definition};
    if (sets.dataOffset > table.size()
        || sets.count
               > (table.size() - sets.dataOffset) / tables::kMaterialRequirementSetRowStride) {
        report_material_requirement_failure("table_bounds", 0, 0);
        return false;
    }
    std::size_t conditionalRows = 0;
    for (std::size_t set = 0; set < sets.count; ++set) {
        const std::size_t setAt = sets.dataOffset + set * tables::kMaterialRequirementSetRowStride;
        auto& output = storage.materialRequirementRows[set];
        output = {};
        std::int64_t relative = 0;
        std::memcpy(&output.requirementSetHash,
                    table.data() + setAt + tables::kMaterialRequirementSetHashOffset,
                    sizeof output.requirementSetHash);
        std::memcpy(&relative,
                    table.data() + setAt + tables::kMaterialRequirementSetArrayPointerOffset,
                    sizeof relative);
        const std::size_t pointerAt = setAt + tables::kMaterialRequirementSetArrayPointerOffset;
        if (output.requirementSetHash == 0 || relative < -static_cast<std::int64_t>(pointerAt)
            || relative > static_cast<std::int64_t>(table.size() - pointerAt)) {
            report_material_requirement_failure("set_pointer", set, 0);
            return false;
        }
        const auto descriptor = static_cast<std::int64_t>(pointerAt) + relative;
        if (descriptor >= 0 && static_cast<std::size_t>(descriptor) <= table.size()
            && table.size() - static_cast<std::size_t>(descriptor) >= kRequirementArrayBytes) {
            const auto descriptorBytes =
                table.subspan(static_cast<std::size_t>(descriptor), kRequirementArrayBytes);
            if (std::all_of(descriptorBytes.begin(), descriptorBytes.end(), [](std::byte value) {
                    return value == std::byte{};
                })) {
                // Native free actions keep a real set hash/index but point at one canonical
                // all-zero array descriptor. Preserve that empty set instead of treating it as
                // a malformed nonempty array.
                output.requirementSetIndex = static_cast<std::uint16_t>(set);
                output.requirementCount = 0;
                continue;
            }
        }
        tables::Array requirements{};
        if (descriptor < 0
            || !tables::find_array_at(table, static_cast<std::size_t>(descriptor), requirements)
            || requirements.elementClass != tables::kMaterialRequirementRowClass
            || requirements.count == 0 || requirements.count > output.requirements.size()
            || requirements.dataOffset > table.size()
            || requirements.count > (table.size() - requirements.dataOffset)
                                        / tables::kMaterialRequirementRowStride) {
            report_material_requirement_failure("set_array", set, 0);
            return false;
        }
        output.requirementSetIndex = static_cast<std::uint16_t>(set);
        output.requirementCount = static_cast<std::uint8_t>(requirements.count);
        for (std::size_t row = 0; row < requirements.count; ++row) {
            const std::size_t at =
                requirements.dataOffset + row * tables::kMaterialRequirementRowStride;
            decltype(generated::MaterialRequirementRow::itemIndex) itemIndex = 0;
            std::uint8_t deleted = 0;
            std::uint8_t omitted = 0;
            std::uint16_t condition = 0;
            auto& requirement = output.requirements[row];
            std::memcpy(&itemIndex,
                        table.data() + at + tables::kMaterialRequirementItemIndexOffset,
                        sizeof itemIndex);
            std::memcpy(&requirement.quantity,
                        table.data() + at + tables::kMaterialRequirementQuantityOffset,
                        sizeof requirement.quantity);
            std::memcpy(&deleted,
                        table.data() + at + tables::kMaterialRequirementDeleteOffset,
                        sizeof deleted);
            std::memcpy(&omitted,
                        table.data() + at + tables::kMaterialRequirementOmitOffset,
                        sizeof omitted);
            std::memcpy(&condition,
                        table.data() + at + tables::kMaterialRequirementSentinelOffset,
                        sizeof condition);
            if (itemIndex >= itemDefinitionCount || deleted > 1 || omitted > 1) {
                report_material_requirement_failure("row", set, row);
                return false;
            }
            requirement.itemDefinitionIndex = itemIndex;
            requirement.condition = condition;
            requirement.deleteOnAction = deleted != 0;
            requirement.omitFromRequirements = omitted != 0;
            conditionalRows += condition != domain::kUnconditionalRequirement ? 1U : 0U;
        }
    }
    const auto definitions =
        std::span(storage.materialRequirementRows).first(static_cast<std::size_t>(sets.count));
    const bool valid = domain::valid(definitions);
    const bool published =
        valid && state::build_data::publish_material_requirement_sets(definitions);
    report_material_requirements(definitions.size(), conditionalRows, valid, published);
    return published;
}

} // namespace sunrise::client::content::items::packages
