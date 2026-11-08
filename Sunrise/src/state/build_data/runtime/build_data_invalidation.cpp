#include "build_data_invalidation.h"

#include <array>
#include <atomic>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808078.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80808a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80808b.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808091.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808099.h"
#include "../runtime.h"
#include "../scenarios/scenario_catalog.h"
#include "../spawn_sets/spawn_set_catalog.h"
#include "../vendors/vendor_catalog.h"
#include "domain_markers.h"

namespace sunrise::state::build_data::invalidation {
namespace {

namespace generated = middleware::content::packages::schema::generated;

struct Row final {
    std::uint32_t classId{};
    std::uint32_t domains{};
    LiveTier tier{};
};

template <typename T> constexpr Row row(std::uint32_t domains) noexcept {
    return {T::kClass, domains, static_cast<LiveTier>(generated::live_tier<T>())};
}

/** Classes each builder reads, with the tier the registry gives them. */
constexpr std::array kRows{
    row<generated::SScenario>(kScenarioLayouts),
    row<generated::SSpawnPoints>(kSpawnSets),
    row<generated::SSpawnPointRow>(kSpawnSets),
    row<generated::SMapPlacements>(kSpawnSets),
    row<generated::SMapContainer>(kSpawnSets),
    row<generated::ContainerMember>(kSpawnSets),
    row<generated::SVendorIndex>(kVendors),
    row<generated::VendorIndexRow>(kVendors),
    row<generated::SInvestmentVendor>(kVendors),
    row<generated::SVendorCost>(kVendors),
};

// A data-tier class must have a rebuild here, or the game would report it applied.
static_assert([] {
    for (const Row& value : kRows) {
        if (value.tier == LiveTier::data && value.domains == kNone) {
            return false;
        }
    }
    return true;
}());

std::atomic<std::uint64_t> g_scenarioRevision{};
std::atomic<std::uint64_t> g_spawnRevision{};
std::atomic<std::uint64_t> g_vendorRevision{};

[[nodiscard]] const Row* find(std::uint32_t classId) noexcept {
    for (const Row& value : kRows) {
        if (value.classId == classId) {
            return &value;
        }
    }
    return nullptr;
}

} // namespace

std::uint32_t domains_for(std::uint32_t classId) noexcept {
    const Row* const found = find(classId);
    return found != nullptr ? found->domains : kNone;
}

LiveTier live_tier(std::uint32_t classId) noexcept {
    const Row* const found = find(classId);
    return found != nullptr ? found->tier : LiveTier::restart;
}

/** Clears each domain's rows, then lifts the saved-cache freeze. */
void invalidate(std::uint32_t domains) noexcept {
    if (domains == kNone) {
        return;
    }
    // Revisions move first, so a builder slice that runs between the clears starts over.
    if ((domains & kScenarioLayouts) != 0) {
        g_scenarioRevision.fetch_add(1, std::memory_order_acq_rel);
        scenarios::clear();
    }
    if ((domains & kSpawnSets) != 0) {
        g_spawnRevision.fetch_add(1, std::memory_order_acq_rel);
        runtime::spawn_catalog::clear();
        spawn_sets::clear();
    }
    if ((domains & kVendors) != 0) {
        g_vendorRevision.fetch_add(1, std::memory_order_acq_rel);
        vendors::clear();
    }
    invalidate_cache();
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=build_data stage=invalidate domains=0x%X",
                      static_cast<unsigned>(domains));
}

/** One counter per domain; an unknown domain reads zero. */
std::uint64_t revision(Domain domain) noexcept {
    switch (domain) {
    case kScenarioLayouts:
        return g_scenarioRevision.load(std::memory_order_acquire);
    case kSpawnSets:
        return g_spawnRevision.load(std::memory_order_acquire);
    case kVendors:
        return g_vendorRevision.load(std::memory_order_acquire);
    default:
        return 0;
    }
}

} // namespace sunrise::state::build_data::invalidation
