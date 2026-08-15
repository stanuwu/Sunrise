#include "platform.h"

#include "coordinator/network_call_coordinator.h"

namespace sunrise::client::hooks::network::platform {
namespace {

using TransportKind = std::int64_t(__fastcall*)();
using AuthenticationStatus = int(__fastcall*)(void*, void*);
using SetCertificate = bool(__fastcall*)(void*, const void*, int, char*);

/** Steam networking availability code accepted by the Client startup gate. */
constexpr int kAvailabilityCurrent = 100;
/** Steam networking code returned when there is no serialized certificate call. */
constexpr int kAvailabilityFailed = -101;
/** Non-SDR socket transport, which the direct BAP route needs. */
constexpr std::int64_t kSocketTransportKind = 1;
/** SDR transport, which the unmodified Client picks. */
constexpr std::int64_t kSdrTransportKind = 2;

/** @return Original transport kind with SDR redirected to direct sockets. */
__declspec(noinline) std::int64_t __fastcall transport_kind_body() noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::transportKind, coordinator::ConsumerKind::none);
    const std::int64_t result = [&]() noexcept {
        const auto call = reinterpret_cast<TransportKind>(lease.original);
        const std::int64_t value = call != nullptr ? call() : kSocketTransportKind;
        return lease.accepting && value == kSdrTransportKind ? kSocketTransportKind : value;
    }();
    coordinator::g_callEgress();
    return result;
}

/**
 * Reports current availability in place of the missing-certificate state.
 * @param self Steam networking authentication state.
 * @param details Optional Steam networking status output.
 * @return The original availability, except the missing-certificate state we handle.
 */
__declspec(noinline) int __fastcall authentication_status_body(void* self, void* details) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::authenticationStatus, coordinator::ConsumerKind::none);
    const int result = [&]() noexcept {
        const auto call = reinterpret_cast<AuthenticationStatus>(lease.original);
        const int value = call != nullptr ? call(self, details) : kAvailabilityFailed;
        return lease.accepting && value == kAvailabilityFailed ? kAvailabilityCurrent : value;
    }();
    coordinator::g_callEgress();
    return result;
}

/**
 * Keeps the parser's side effects, and still accepts the local Server certificate.
 * @param self Steam networking authentication state.
 * @param certificate Serialized certificate bytes.
 * @param certificateSize Certificate size in bytes.
 * @param error Caller-owned parser error storage.
 * @return True when the original parser entry exists.
 */
__declspec(noinline) bool __fastcall set_certificate_body(void* self,
                                                          const void* certificate,
                                                          int certificateSize,
                                                          char* error) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::setCertificate, coordinator::ConsumerKind::none);
    const bool result = [&]() noexcept {
        const auto call = reinterpret_cast<SetCertificate>(lease.original);
        if (call == nullptr) {
            return false;
        }
        const bool accepted = call(self, certificate, certificateSize, error);
        return lease.accepting ? true : accepted;
    }();
    coordinator::g_callEgress();
    return result;
}

} // namespace

/** @return The body itself. Internal linkage, so it cannot be a linker thunk. */
void* transport_kind_entry_point() noexcept {
    return reinterpret_cast<void*>(&transport_kind_body);
}

/** @return The body itself. Internal linkage, so it cannot be a linker thunk. */
void* authentication_status_entry_point() noexcept {
    return reinterpret_cast<void*>(&authentication_status_body);
}

/** @return The body itself. Internal linkage, so it cannot be a linker thunk. */
void* set_certificate_entry_point() noexcept {
    return reinterpret_cast<void*>(&set_certificate_body);
}

} // namespace sunrise::client::hooks::network::platform
