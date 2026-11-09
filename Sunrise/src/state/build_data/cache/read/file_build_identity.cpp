#include <Windows.h>

#include "../internal.h"

namespace sunrise::state::build_data::cache {

/** Reads the deployed content image's PE identity without loading or executing it. */
bool file_build_identity(const wchar_t* path,
                         std::uint64_t configuredEquipmentHash,
                         BuildIdentity& identity) noexcept {
    identity = {};
    if (path == nullptr) {
        return false;
    }
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    DWORD read{};
    bool valid =
        GetFileSizeEx(file, &size) != FALSE
        && ReadFile(file, &dos, sizeof dos, &read, nullptr) != FALSE && read == sizeof dos
        && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= static_cast<LONG>(sizeof dos)
        && size.QuadPart >= static_cast<LONGLONG>(dos.e_lfanew) + static_cast<LONGLONG>(sizeof nt);
    if (valid) {
        LARGE_INTEGER offset{};
        offset.QuadPart = dos.e_lfanew;
        valid = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) != FALSE
                && ReadFile(file, &nt, sizeof nt, &read, nullptr) != FALSE && read == sizeof nt
                && nt.Signature == IMAGE_NT_SIGNATURE
                && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
                && nt.FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)
                && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
                && nt.OptionalHeader.SizeOfImage != 0;
    }
    valid = CloseHandle(file) != FALSE && valid;
    if (valid) {
        identity = {
            nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage, configuredEquipmentHash};
    }
    return valid;
}

} // namespace sunrise::state::build_data::cache
