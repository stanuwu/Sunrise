#include "investment_constant_catalog.h"

#include "../table.h"

namespace sunrise::state::build_data::constants {
namespace {

// One row, not a table, so it holds the value directly under the shared Lock.
Lock g_lock;
InvestmentConstants g_constants{};

} // namespace

/** Clears the published investment constants under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_constants = {};
}

/** Publishes one extracted constants row. */
bool replace(const InvestmentConstants& value) noexcept {
    if (!value.extracted) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    g_constants = value;
    return true;
}

/** @param value Receives the published constants. @return True when a row is published. */
bool find(InvestmentConstants& value) noexcept {
    const Lock::Shared guard(g_lock);
    value = g_constants;
    return value.extracted;
}

/** @return Copy read under the lock, cleared when nothing is published. */
InvestmentConstants snapshot() noexcept {
    InvestmentConstants value{};
    (void)find(value);
    return value;
}

} // namespace sunrise::state::build_data::constants
