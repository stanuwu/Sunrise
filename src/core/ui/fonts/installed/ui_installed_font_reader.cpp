#include "ui_installed_font_reader.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../../filesystem/path.h"

namespace sunrise::core::ui::fonts::installed {
namespace {

/** One KiB, the unit the fixed font size policy uses. */
constexpr std::size_t kKibibyteBytes = 1024;
/** 160 KiB holds the installed 143,564-byte face and small build drift. */
constexpr std::size_t kFontCapacityKibibytes = 160;
/** Total installed-font storage for one optional face. */
constexpr std::size_t kFontCapacityBytes = kFontCapacityKibibytes * kKibibyteBytes;
/** An OpenType tag is 4 file bytes. */
constexpr std::size_t kOpenTypeTagBytes = 4;
/** Each unsigned field in the OpenType offset table is 2 file bytes. */
constexpr std::size_t kOpenTypeFieldBytes = 2;
/** One OpenType table directory record is 16 bytes. */
constexpr std::size_t kOpenTypeTableRecordBytes = 16;
/** OpenType stores one byte as 8 bits, high to low. */
constexpr unsigned int kBitsPerByte = 8;
/** The installed CFF-based OpenType face starts with the standard OTTO tag. */
constexpr std::array<char, kOpenTypeTagBytes> kOpenTypeCffSignature{'O', 'T', 'T', 'O'};
/** The optional UI face lives in the game install's existing font directory. */
constexpr std::wstring_view kInstalledFontSuffix = L"fonts\\NeueHaasUnicaW1G-Regular.otf";

/** Byte arrays keep the file byte order of the OpenType offset-table fields. */
struct OpenTypeOffsetTable {
    std::array<char, kOpenTypeTagBytes> signature{};
    std::array<std::byte, kOpenTypeFieldBytes> tableCount{};
    std::array<std::byte, kOpenTypeFieldBytes> searchRange{};
    std::array<std::byte, kOpenTypeFieldBytes> entrySelector{};
    std::array<std::byte, kOpenTypeFieldBytes> rangeShift{};
};

/** The OpenType offset table is 12 bytes, before the table records. */
constexpr std::size_t kOpenTypeOffsetTableBytes = 12;
/** One offset table and one directory record are the smallest possible font file. */
constexpr std::size_t kMinimumFontBytes = kOpenTypeOffsetTableBytes + kOpenTypeTableRecordBytes;
static_assert(sizeof(OpenTypeOffsetTable) == kOpenTypeOffsetTableBytes);

alignas(std::max_align_t) std::array<std::byte, kFontCapacityBytes> g_fontBytes{};
std::size_t g_fontByteCount{};
core::path::Buffer g_pathScratch{};

/**
 * Finds the folder of the process image or of one loaded module.
 * @param module Null for the process image, or one loaded fallback module.
 * @return True when the whole folder and its trailing separator fit fixed storage.
 */
[[nodiscard]] bool resolve_image_directory(HMODULE module) noexcept {
    const DWORD copied = GetModuleFileNameW(
        module, g_pathScratch.chars.data(), static_cast<DWORD>(g_pathScratch.chars.size()));
    if (copied == 0 || copied >= g_pathScratch.chars.size()) {
        return false;
    }

    g_pathScratch.length = copied;
    while (g_pathScratch.length != 0 && g_pathScratch.chars[g_pathScratch.length - 1] != L'\\') {
        --g_pathScratch.length;
    }
    if (g_pathScratch.length == 0) {
        return false;
    }
    g_pathScratch.chars[g_pathScratch.length] = L'\0';
    return true;
}

/**
 * Decodes one 2-byte OpenType unsigned value.
 * @param bytes Big-endian field bytes.
 * @return The value in host order.
 */
[[nodiscard]] std::uint16_t
read_big_endian_u16(const std::array<std::byte, kOpenTypeFieldBytes>& bytes) noexcept {
    const auto high = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0]));
    const auto low = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1]));
    return static_cast<std::uint16_t>((high << kBitsPerByte) | low);
}

/**
 * Checks the fixed header before the bytes reach Dear ImGui's font parser.
 * @param bytes Complete candidate file bytes.
 * @return True for the expected CFF OpenType tag and a complete table directory.
 */
[[nodiscard]] bool valid_open_type(const std::byte* bytes, std::size_t byteCount) noexcept {
    if (bytes == nullptr || byteCount < sizeof(OpenTypeOffsetTable)) {
        return false;
    }

    OpenTypeOffsetTable header{};
    std::memcpy(&header, bytes, sizeof header);
    if (header.signature != kOpenTypeCffSignature) {
        return false;
    }

    const std::size_t tableCount = read_big_endian_u16(header.tableCount);
    if (tableCount == 0) {
        return false;
    }
    const std::size_t directoryBytes = sizeof header + (tableCount * kOpenTypeTableRecordBytes);
    return directoryBytes <= byteCount;
}

/**
 * Reads one open file only when its whole size fits fixed storage.
 * @param file Readable Windows file handle with a stable share mode.
 * @return True when one exact, valid OpenType file was read.
 */
[[nodiscard]] bool read_exact_font(HANDLE file) noexcept {
    LARGE_INTEGER fileSize{};
    if (GetFileSizeEx(file, &fileSize) == FALSE || fileSize.QuadPart < 0) {
        return false;
    }

    const auto byteCount = static_cast<unsigned long long>(fileSize.QuadPart);
    if (byteCount < kMinimumFontBytes || byteCount > g_fontBytes.size()
        || byteCount > static_cast<unsigned long long>((std::numeric_limits<DWORD>::max)())
        || byteCount > static_cast<unsigned long long>((std::numeric_limits<int>::max)())) {
        return false;
    }

    const DWORD expectedBytes = static_cast<DWORD>(byteCount);
    DWORD bytesRead{};
    if (ReadFile(file, g_fontBytes.data(), expectedBytes, &bytesRead, nullptr) == FALSE
        || bytesRead != expectedBytes) {
        return false;
    }

    LARGE_INTEGER finalSize{};
    if (GetFileSizeEx(file, &finalSize) == FALSE || finalSize.QuadPart != fileSize.QuadPart
        || !valid_open_type(g_fontBytes.data(), bytesRead)) {
        return false;
    }
    g_fontByteCount = bytesRead;
    return true;
}

/** Wipes the temporary absolute path after each open attempt. */
void clear_path_scratch() noexcept {
    SecureZeroMemory(&g_pathScratch, sizeof g_pathScratch);
}

/**
 * Tries one image-relative path. Bytes are kept only after a full read and close.
 * @param module Null for the process image, or one loaded fallback module.
 * @return True when the whole optional face was loaded.
 */
[[nodiscard]] bool try_load(HMODULE module) noexcept {
    if (!resolve_image_directory(module)
        || !core::path::append(g_pathScratch, kInstalledFontSuffix)) {
        clear_path_scratch();
        return false;
    }

    const HANDLE file = CreateFileW(g_pathScratch.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    clear_path_scratch();
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const bool loaded = read_exact_font(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (!loaded || !closed) {
        SecureZeroMemory(g_fontBytes.data(), g_fontBytes.size());
        g_fontByteCount = 0;
        return false;
    }
    return true;
}

} // namespace

/** Reads the optional face from the process directory, then one module directory. */
bool load(HMODULE module, DataView& output) noexcept {
    output = {};
    clear();
    if (!try_load(nullptr) && (module == nullptr || !try_load(module))) {
        clear();
        return false;
    }

    output.bytes = g_fontBytes.data();
    output.byteCount = static_cast<int>(g_fontByteCount);
    return true;
}

/** Wipes the installed-font bytes and all path scratch storage. */
void clear() noexcept {
    SecureZeroMemory(g_fontBytes.data(), g_fontBytes.size());
    g_fontByteCount = 0;
    clear_path_scratch();
}

/** @return Number of installed-font bytes kept for the active atlas. */
std::size_t byte_count() noexcept {
    return g_fontByteCount;
}

} // namespace sunrise::core::ui::fonts::installed
