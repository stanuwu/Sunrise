#include "coop_admission.h"

#include <array>

#include "../../core/network_capacity.h"

namespace sunrise::server::bap::coop_admission {
namespace {

/** One admitted account and the address it passed from. */
struct Entry final {
    std::uint64_t primarySoid{};
    std::uint32_t address{};
};

/** One row per enrollable player; a re-admission replaces its own row. */
std::array<Entry, core::network_capacity::kPlayers> g_entries{};

} // namespace

bool admit(std::uint64_t primarySoid, std::uint32_t address) noexcept {
    Entry* free = nullptr;
    for (Entry& entry : g_entries) {
        if (entry.primarySoid == primarySoid) {
            entry.address = address;
            return true;
        }
        if (free == nullptr && entry.primarySoid == 0) {
            free = &entry;
        }
    }
    if (free == nullptr || primarySoid == 0) {
        return false;
    }
    *free = {primarySoid, address};
    return true;
}

bool admitted(std::uint64_t primarySoid, std::uint32_t address) noexcept {
    for (const Entry& entry : g_entries) {
        if (entry.primarySoid != 0 && entry.primarySoid == primarySoid) {
            return entry.address == address;
        }
    }
    return false;
}

void reset() noexcept {
    g_entries = {};
}

} // namespace sunrise::server::bap::coop_admission