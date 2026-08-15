#include <algorithm>
#include <array>

#include "validation.h"

namespace sunrise::state::entitlements {
namespace {

/** One bundled definition in handle order. */
struct Authored final {
    std::string_view name;
    Ownership ownership;
};

/**
 * The bundled ownership policy, in handle order. The first 3 are local: the base sku, the paid
 * tier, and the restriction that must be defined but never owned. The rest are the installed DLC,
 * named by public application id.
 */
constexpr std::array kAuthored{
    Authored{"1085660", Ownership::handle},         Authored{"STEAM_PAID_TIER", Ownership::handle},
    Authored{"STEAM_UGC_BLOCKED", Ownership::none}, Authored{"1090090", Ownership::application},
    Authored{"1090091", Ownership::application},    Authored{"1090092", Ownership::application},
    Authored{"1090093", Ownership::application},    Authored{"1090094", Ownership::application},
    Authored{"1090095", Ownership::application},    Authored{"1090096", Ownership::application},
    Authored{"1090150", Ownership::application},    Authored{"1090151", Ownership::application},
    Authored{"1090152", Ownership::application},    Authored{"1090170", Ownership::application},
    Authored{"1090171", Ownership::application},    Authored{"1090200", Ownership::application},
    Authored{"1090201", Ownership::application},    Authored{"1090202", Ownership::application},
    Authored{"1330040", Ownership::application},
};
static_assert(kAuthored.size() <= kCapacity);

} // namespace

/** Supplies the ownership policy used when settings define none. */
Table authored() noexcept {
    Table table{};
    for (const Authored& source : kAuthored) {
        Entitlement& target = table.entries[table.count++];
        std::copy(source.name.begin(), source.name.end(), target.name.begin());
        target.nameLength = static_cast<std::uint8_t>(source.name.size());
        target.ownership = source.ownership;
    }
    return table;
}

} // namespace sunrise::state::entitlements
