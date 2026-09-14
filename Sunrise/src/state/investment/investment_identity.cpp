#include <array>
#include <memory>
#include <new>

#include "store_internal.h"

namespace sunrise::state::investment::store {

bool bind_identity(std::uint64_t primarySoid) noexcept {
    // Account soid written into the bundled seed database. A database still carrying it has
    // never been bound to an installation, so it is rebound to this player's identity once;
    // any other soid belongs to a different player and is refused.
    constexpr std::uint64_t kUpstreamSeedSoid = 0x9EAA300100100100ULL;
    if (primarySoid == 0 || (primarySoid & 0xFFULL) != 0) {
        return false;
    }
    Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    std::uint64_t current = 0;
    Statement accountRow("SELECT soid FROM account WHERE id=1");
    if (accountRow.step() != SQLITE_ROW || !accountRow.column(0, current)
        || accountRow.step() != SQLITE_DONE) {
        return false;
    }
    if (current == primarySoid) {
        return transaction.commit();
    }
    if (current != kUpstreamSeedSoid) {
        return false;
    }

    std::array<std::size_t, kCharacterCapacity> slots{};
    std::size_t count = 0;
    {
        Statement characters("SELECT slot FROM characters ORDER BY slot");
        int result = characters.step();
        while (result == SQLITE_ROW) {
            std::size_t slot = 0;
            if (count == slots.size() || !characters.column(0, slot)
                || slot >= kCharacterCapacity) {
                return false;
            }
            slots[count++] = slot;
            result = characters.step();
        }
        if (result != SQLITE_DONE) {
            return false;
        }
    }
    // Finalize the read before updating that table on this connection.
    Statement accountUpdate("UPDATE account SET soid=? WHERE id=1");
    Statement characterUpdate("UPDATE characters SET soid=? WHERE slot=?");
    if (!accountUpdate.write(primarySoid)) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!characterUpdate.write(primarySoid + slots[index] + 1, slots[index])) {
            return false;
        }
    }
    const std::unique_ptr<AccountState> candidate{new (std::nothrow) AccountState{}};
    return candidate && read_account(*candidate) && transaction.commit();
}

} // namespace sunrise::state::investment::store
