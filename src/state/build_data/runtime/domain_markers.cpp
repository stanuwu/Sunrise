#include "domain_markers.h"

#include <Windows.h>

namespace sunrise::state::build_data::runtime {
namespace {

/** Publish marker for one generated domain, guarded by its own State lock. */
class DomainMarker final {
public:
    /** Clears the marker under its lock. */
    void clear() noexcept {
        AcquireSRWLockExclusive(&lock_);
        ready_ = false;
        ReleaseSRWLockExclusive(&lock_);
    }

    /** Marks the domain complete under its lock. */
    void publish() noexcept {
        AcquireSRWLockExclusive(&lock_);
        ready_ = true;
        ReleaseSRWLockExclusive(&lock_);
    }

    /** @return The publish state, read under the lock. */
    [[nodiscard]] bool ready() noexcept {
        AcquireSRWLockShared(&lock_);
        const bool result = ready_;
        ReleaseSRWLockShared(&lock_);
        return result;
    }

private:
    SRWLOCK lock_{SRWLOCK_INIT};
    bool ready_{};
};

DomainMarker g_abilityBuckets;
DomainMarker g_details;
DomainMarker g_named;
DomainMarker g_spawnCatalog;
DomainMarker g_bubbleCatalog;

} // namespace

namespace ability_buckets {

void clear() noexcept {
    g_abilityBuckets.clear();
}

void publish() noexcept {
    g_abilityBuckets.publish();
}

bool ready() noexcept {
    return g_abilityBuckets.ready();
}

} // namespace ability_buckets

namespace details {

void clear() noexcept {
    g_details.clear();
}

void publish() noexcept {
    g_details.publish();
}

bool ready() noexcept {
    return g_details.ready();
}

} // namespace details

namespace name_catalog {

void clear() noexcept {
    g_bubbleCatalog.clear();
}

void publish() noexcept {
    g_bubbleCatalog.publish();
}

bool ready() noexcept {
    return g_bubbleCatalog.ready();
}

} // namespace name_catalog

namespace spawn_catalog {

void clear() noexcept {
    g_spawnCatalog.clear();
}

void publish() noexcept {
    g_spawnCatalog.publish();
}

bool ready() noexcept {
    return g_spawnCatalog.ready();
}

} // namespace spawn_catalog

namespace named {

void clear() noexcept {
    g_named.clear();
}

void publish() noexcept {
    g_named.publish();
}

bool ready() noexcept {
    return g_named.ready();
}

} // namespace named

} // namespace sunrise::state::build_data::runtime
