#include "platform_identity.h"

#include <Windows.h>

#include <bcrypt.h>

#include "../../../state/account/account_platform.h"
#include "../../filesystem/path.h"

#pragma comment(lib, "bcrypt.lib")

namespace sunrise::core::settings::steam::platform_identity {
namespace {
struct IdentityRecord {
    std::uint32_t magic{0x31444953};
    std::uint32_t version{1};
    std::uint64_t token{};
    std::uint64_t inverse{};
};
static_assert(sizeof(IdentityRecord) == 24);

} // namespace

/** Create once; an existing invalid cache never silently becomes another account. */
bool load_or_create(std::uint64_t& token) noexcept {
    path::Buffer filename;
    if (!path::artifact_file(L"cache\\platform_identity.bin", filename)) {
        return false;
    }
    IdentityRecord record;
    HANDLE file = CreateFileW(filename.chars.data(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER length{};
        DWORD read = 0;
        const bool okay =
            GetFileSizeEx(file, &length) && length.QuadPart == static_cast<LONGLONG>(sizeof record)
            && ReadFile(file, &record, sizeof record, &read, nullptr) && read == sizeof record;
        const bool closed = CloseHandle(file) != FALSE;
        if (!okay || !closed || record.magic != 0x31444953 || record.version != 1
            || record.inverse != ~record.token || !state::account::platform::valid(record.token)) {
            return false;
        }
        token = record.token;
        return true;
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return false;
    }
    std::uint32_t random = 0;
    if (BCryptGenRandom(nullptr,
                        reinterpret_cast<PUCHAR>(&random),
                        sizeof random,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)
        < 0) {
        return false;
    }
    if ((random & 0xFFFFFF00U) == 0) {
        random = 0x100;
    }

    // Same shape as the default id: universe 1, individual account, instance 1 in the high
    // dword, a random 24-bit account number, and the default id's low byte.
    record.token = 0x0110000100000000ULL | (random & 0xFFFFFF00U) | 0xC5U;
    record.inverse = ~record.token;
    file = CreateFileW(filename.chars.data(),
                       GENERIC_WRITE,
                       0,
                       nullptr,
                       CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool okay = WriteFile(file, &record, sizeof record, &written, nullptr)
                      && written == sizeof record && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (!okay || !closed) {
        return false;
    }
    token = record.token;
    return true;
}

} // namespace sunrise::core::settings::steam::platform_identity
