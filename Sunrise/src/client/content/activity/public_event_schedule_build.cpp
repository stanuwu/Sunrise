#include "public_event_schedule_build.h"

#include <array>
#include <vector>

#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../middleware/content/packages/tables/public_event_schedule_table.h"
#include "../../../state/build_data/public_event_schedule/definition.h"
#include "../items/packages/internal.h"

namespace sunrise::client::content::activity {
namespace {
namespace packages = middleware::content::packages;
namespace schedule = packages::tables::public_event_schedule;
} // namespace

bool build_public_event_schedule(const packages::reader::Source& source,
                                 packages::reader::Scratch& scratch) noexcept {
    namespace store = state::build_data::public_event_schedule;
    if (store::ready()) {
        return true;
    }
    std::array<std::uint32_t, items::packages::kContainerCandidates> candidates{};
    std::size_t candidateCount{};
    if (!items::packages::investment_globals_tags(candidates, candidateCount)) {
        return false;
    }
    std::vector<std::byte> globals, payload;
    store::Table table{};
    for (std::size_t i = 0; i < candidateCount; ++i) {
        std::uint32_t tag{};
        if (!packages::reader::read_tag(source, scratch, candidates[i], globals)
            || !packages::tables::child_tag(globals, schedule::kGlobalsChild, tag)
            || !packages::reader::read_tag(source, scratch, tag, payload)
            || !schedule::decode(payload, tag, table) || !store::publish(table)) {
            continue;
        }
        return true;
    }
    return false;
}
} // namespace sunrise::client::content::activity
