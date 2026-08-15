#include <Windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../internal.h"

namespace sunrise::steam::interfaces::methods {
namespace {

/** Steam callback id for a finished app ticket request. */
constexpr int kEncryptedAppTicketCallback = 154;
/** Steam result code for success. */
constexpr int kResultOk = 1;
/**
 * Ticket length. Steam signs the real ticket, so it cannot be made here. The only reader is the
 * in-process Server, which puts the ticket in the SignOn reply and reads no field from it.
 */
constexpr DWORD kTicketSize = 128;

/** Payload handed to the caller's call-result object. */
struct EncryptedAppTicketResponse final {
    int result{};
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<unsigned char, kTicketSize> g_ticket{};
bool g_generated{};

/** @return True once the ticket bytes exist. */
[[nodiscard]] bool ensure_ticket() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_generated) {
        g_generated = BCryptGenRandom(nullptr,
                                      g_ticket.data(),
                                      static_cast<ULONG>(g_ticket.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG)
                      == 0;
    }
    const bool ready = g_generated;
    ReleaseSRWLockExclusive(&g_lock);
    return ready;
}

} // namespace

/**
 * Finishes one encrypted app ticket request through the call-result registry.
 * @param data Caller payload. Steam puts it inside the real ticket.
 * @return API call id, or zero when the ticket or the queue is not ready.
 */
ApiCall request_encrypted_app_ticket([[maybe_unused]] void* self,
                                     [[maybe_unused]] const void* data,
                                     [[maybe_unused]] int dataSize) noexcept {
    if (!ensure_ticket()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=steam stage=app_ticket_request result=fail reason=material");
        return 0;
    }
    const ApiCall call = next_api_call();
    const EncryptedAppTicketResponse response{kResultOk};
    if (!queue_callback(kEncryptedAppTicketCallback, call, &response, sizeof(response))) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=steam stage=app_ticket_request result=fail reason=queue");
        return 0;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=steam stage=app_ticket_request result=ok");
    return call;
}

/**
 * Copies the finished ticket into caller storage.
 * @param maxTicket Caller storage size in bytes.
 * @param ticketSize Receives the copied byte count.
 * @return True when the whole ticket fits.
 */
bool get_encrypted_app_ticket([[maybe_unused]] void* self,
                              void* ticket,
                              int maxTicket,
                              DWORD* ticketSize) noexcept {
    if (ticketSize != nullptr) {
        *ticketSize = 0;
    }
    if (ticket == nullptr || maxTicket < static_cast<int>(kTicketSize) || ticketSize == nullptr
        || !ensure_ticket()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=steam stage=app_ticket_read result=fail reason=capacity");
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    std::memcpy(ticket, g_ticket.data(), kTicketSize);
    ReleaseSRWLockShared(&g_lock);
    *ticketSize = kTicketSize;
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=steam stage=app_ticket_read result=ok");
    return true;
}

} // namespace sunrise::steam::interfaces::methods
