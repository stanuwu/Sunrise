#include "activity_transport_fields.h"

#include <Windows.h>

#include <atomic>

namespace sunrise::state::activity::membership {
namespace {

struct Record final {
    TransportFields fields{};
    std::uint64_t memberKey{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    bool occupied{};
};

std::array<Record, kTransportFieldCapacity> g_records{};
SRWLOCK g_lock{SRWLOCK_INIT};
std::atomic<std::uint64_t> g_generation{};

[[nodiscard]] bool same(const TransportFields& left, const TransportFields& right) noexcept {
    return left.flags == right.flags && left.hasFlags == right.hasFlags
           && left.hasAddress == right.hasAddress && left.hasAddressAlt == right.hasAddressAlt
           && left.address == right.address && left.addressAlt == right.addressAlt;
}

[[nodiscard]] bool carries_address(const TransportFields& fields) noexcept {
    return fields.hasAddress || fields.hasAddressAlt;
}

} // namespace

bool observe_transport_fields(std::uint64_t memberKey,
                              std::uint64_t accountSoid,
                              std::uint64_t characterSoid,
                              const TransportFields& fields) noexcept {
    if (memberKey == 0) {
        return false;
    }
    bool changed = true;
    AcquireSRWLockExclusive(&g_lock);
    std::size_t target = kTransportFieldCapacity;
    for (std::size_t index = 0; index < kTransportFieldCapacity; ++index) {
        if (g_records[index].occupied && g_records[index].memberKey == memberKey) {
            target = index;
            break;
        }
        if (!g_records[index].occupied && target == kTransportFieldCapacity) {
            target = index;
        }
    }
    if (target == kTransportFieldCapacity) {
        // A full table drops the oldest record; the newest members are the live ones.
        for (std::size_t index = 1; index < kTransportFieldCapacity; ++index) {
            g_records[index - 1] = g_records[index];
        }
        target = kTransportFieldCapacity - 1;
        g_records[target] = {};
    }
    const bool reuses = g_records[target].occupied && g_records[target].memberKey == memberKey;
    if (reuses) {
        changed = !same(g_records[target].fields, fields)
                  || g_records[target].accountSoid != accountSoid
                  || g_records[target].characterSoid != characterSoid;
    }
    // The caller has already merged each connection's sparse report and selected its live image.
    // Retaining absent fields here would resurrect a carrier from a retired connection.
    g_records[target].fields = fields;
    g_records[target].memberKey = memberKey;
    g_records[target].accountSoid = accountSoid;
    g_records[target].characterSoid = characterSoid;
    g_records[target].occupied = true;
    if (changed) {
        g_generation.fetch_add(1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return changed;
}

void forget_transport_fields(std::uint64_t memberKey) noexcept {
    if (memberKey == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (Record& record : g_records) {
        if (record.occupied && record.memberKey == memberKey) {
            record = {};
            g_generation.fetch_add(1, std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::uint64_t transport_fields_generation() noexcept {
    return g_generation.load(std::memory_order_acquire);
}

bool transport_fields_exact(std::uint64_t memberKey, TransportFields& fields) noexcept {
    fields = {};
    if (memberKey == 0) {
        return false;
    }
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    for (const Record& record : g_records) {
        if (!record.occupied || record.memberKey != memberKey || !carries_address(record.fields)) {
            continue;
        }
        fields = record.fields;
        found = true;
        break;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

bool transport_fields_for_account(std::uint64_t accountSoid,
                                  std::uint64_t characterSoid,
                                  TransportFields& fields) noexcept {
    fields = {};
    if (accountSoid == 0) {
        return false;
    }
    bool found = false;
    bool conflicted = false;
    AcquireSRWLockShared(&g_lock);
    for (const Record& record : g_records) {
        if (!record.occupied || record.accountSoid != accountSoid
            || (characterSoid != 0 && record.characterSoid != characterSoid)
            || !carries_address(record.fields)) {
            continue;
        }
        if (found && !same(fields, record.fields)) {
            conflicted = true;
            break;
        }
        fields = record.fields;
        found = true;
    }
    ReleaseSRWLockShared(&g_lock);
    if (conflicted) {
        fields = {};
        return false;
    }
    return found;
}

} // namespace sunrise::state::activity::membership
