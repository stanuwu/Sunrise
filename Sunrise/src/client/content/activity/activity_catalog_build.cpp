#include "activity_catalog_build.h"

#include "../../../middleware/content/packages/tables/activity_table.h"
#include "../items/packages/internal.h"
#include "activity_presentation_build.h"

namespace sunrise::client::content::activity {
namespace packages = middleware::content::packages;
bool build_catalog(const packages::reader::Source& source,
                   packages::reader::Scratch& scratch) noexcept {
    namespace catalog = state::build_data::activities;
    namespace tables = packages::tables;
    if (catalog::ready()) {
        return true;
    }
    if (catalog::extraction_failed()) {
        return false;
    }
    std::array<std::uint32_t, items::packages::kContainerCandidates> candidates{};
    std::size_t candidateCount{};
    if (!items::packages::investment_globals_tags(candidates, candidateCount)) {
        catalog::note_extraction_failure();
        return false;
    }
    std::vector<std::byte> globals, root, table;
    static std::array<catalog::Definition, catalog::kCapacity> rows{};
    for (std::size_t i = 0; i < candidateCount; ++i) {
        std::uint32_t rootTag{}, tableTag{}, rootClass{};
        std::size_t count{};
        if (packages::reader::read_tag(source, scratch, candidates[i], globals)
            && tables::child_tag(globals, tables::kInvestmentRootChild, rootTag)
            && packages::reader::read_tag(source, scratch, rootTag, root, rootClass)
            && rootClass == tables::kInvestmentRootClass
            && tables::slot_tag(root, tables::activities::kRootSlot, tableTag)
            && packages::reader::read_tag(source, scratch, tableTag, table)
            && tables::activities::decode(table, rows, count)) {
            const auto found = std::span(rows).first(count);
            build_presentations(source, scratch, globals, found);
            build_artwork(source, scratch);
            return catalog::publish(found);
        }
    }
    catalog::note_extraction_failure();
    return false;
}
} // namespace sunrise::client::content::activity
