#include "peer_routes.h"

#include <Windows.h>

#include <algorithm>
#include <array>

namespace sunrise::state::network::peer_routes {
namespace {
SRWLOCK lock = SRWLOCK_INIT;
std::array<Endpoint, kCapacity> routes{};
std::size_t count{};
} // namespace
bool replace(std::span<const Endpoint> endpoints) noexcept {
    if (endpoints.size() > kCapacity
        || !std::all_of(endpoints.begin(),
                        endpoints.end(),
                        middleware::gameplay::descriptor::unicast_endpoint)) {
        return false;
    }
    AcquireSRWLockExclusive(&lock);
    routes = {};
    std::copy(endpoints.begin(), endpoints.end(), routes.begin());
    count = endpoints.size();
    ReleaseSRWLockExclusive(&lock);
    return true;
}
bool allows(Endpoint endpoint) noexcept {
    AcquireSRWLockShared(&lock);
    const bool found =
        std::find(routes.begin(), routes.begin() + static_cast<std::ptrdiff_t>(count), endpoint)
        != routes.begin() + static_cast<std::ptrdiff_t>(count);
    ReleaseSRWLockShared(&lock);
    return found;
}
void reset() noexcept {
    AcquireSRWLockExclusive(&lock);
    routes = {};
    count = 0;
    ReleaseSRWLockExclusive(&lock);
}
} // namespace sunrise::state::network::peer_routes
