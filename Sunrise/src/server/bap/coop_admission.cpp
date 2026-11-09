#include "coop_admission.h"

#include <array>

#include "../../core/network_capacity.h"

namespace sunrise::server::bap::coop_admission {
namespace {

/** One admitted account, the address it reported from and its sign-on keys. */
struct Entry final {
    std::uint64_t primarySoid{};
    std::uint32_t address{};
    state::SignOnState keys{};
};

/** One row per enrollable player; a new report replaces its own row. */
std::array<Entry, core::network_capacity::kPlayers> g_entries{};

} // namespace

bool admit(std::uint64_t primarySoid,
           std::uint32_t address,
           const state::SignOnState& keys) noexcept {
    if (primarySoid == 0) {
        return false;
    }
    Entry* free = nullptr;
    for (Entry& entry : g_entries) {
        if (entry.primarySoid == primarySoid) {
            free = &entry;
            break;
        }
        if (free == nullptr && entry.primarySoid == 0) {
            free = &entry;
        }
    }
    if (free == nullptr) {
        return false;
    }
    free->primarySoid = primarySoid;
    free->address = address;
    free->keys.encryptionKey = keys.encryptionKey;
    free->keys.authenticationKey = keys.authenticationKey;
    return true;
}

bool keys(std::uint64_t primarySoid, std::uint32_t address, state::SignOnState& output) noexcept {
    for (const Entry& entry : g_entries) {
        if (entry.primarySoid != 0 && entry.primarySoid == primarySoid) {
            if (entry.address != address) {
                return false;
            }
            output.encryptionKey = entry.keys.encryptionKey;
            output.authenticationKey = entry.keys.authenticationKey;
            return true;
        }
    }
    return false;
}

} // namespace sunrise::server::bap::coop_admission