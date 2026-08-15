#include <Windows.h>

#include <array>
#include <cwchar>
#include <limits>

#include "handle_cache.h"

namespace sunrise::middleware::content::packages::reader::handle_cache {
namespace {

/** Shared files kept open for the class sweeps, which run once and on one thread. */
constexpr std::size_t kSharedSlots = 8;
/** Standard 64-bit FNV-1a offset basis starts the path key. */
constexpr std::uint64_t kHashBasis = 14695981039346656037ULL;
/** Standard 64-bit FNV-1a prime mixes each path character. */
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<FileSlot, kSharedSlots> g_slots{};
std::uint64_t g_useCounter{};

/** @param path Full package path. @return Its key. */
[[nodiscard]] std::uint64_t path_hash(const Path& path) noexcept {
    std::uint64_t hash = kHashBasis;
    for (const wchar_t character : path.chars) {
        if (character == L'\0') {
            break;
        }
        hash ^= static_cast<std::uint64_t>(character);
        hash *= kHashPrime;
    }
    return hash;
}

/**
 * Finds the open file for one path in one set of slots. Opens it when no slot holds it.
 * @param slots Slots to search and replace within.
 * @param counter Use counter of those slots.
 * @param path Full package path.
 * @return The open file, or null when the path cannot be opened.
 */
[[nodiscard]] HANDLE
acquire(std::span<FileSlot> slots, std::uint64_t& counter, const Path& path) noexcept {
    const std::uint64_t hash = path_hash(path);
    for (FileSlot& slot : slots) {
        // The hash picks the slot and the text confirms it, so no read can reach another file.
        if (slot.file != nullptr && slot.hash == hash
            && std::wcscmp(slot.path.chars.data(), path.chars.data()) == 0) {
            slot.used = ++counter;
            return slot.file;
        }
    }
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    FileSlot* target = &slots[0];
    for (FileSlot& slot : slots) {
        if (slot.file == nullptr) {
            target = &slot;
            break;
        }
        if (slot.used < target->used) {
            target = &slot;
        }
    }
    if (target->file != nullptr) {
        (void)CloseHandle(target->file);
    }
    target->path = path;
    target->file = file;
    target->hash = hash;
    target->used = ++counter;
    return file;
}

/**
 * Reads one positioned range from an open file.
 * @param file Open package file.
 * @param offset File offset.
 * @param output Exact destination.
 * @return True when every requested byte is read.
 */
[[nodiscard]] bool
read_positioned(HANDLE file, std::uint64_t offset, std::span<std::byte> output) noexcept {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    DWORD read = 0;
    return ReadFile(file, output.data(), static_cast<DWORD>(output.size()), &read, &position)
               != FALSE
           && read == output.size();
}

/** @param output Requested range. @return True when it can be read in one call. */
[[nodiscard]] bool readable(std::span<std::byte> output) noexcept {
    return !output.empty() && output.size() <= (std::numeric_limits<DWORD>::max)();
}

/** @param slots Slots whose files are closed. */
void close_slots(std::span<FileSlot> slots) noexcept {
    for (FileSlot& slot : slots) {
        if (slot.file != nullptr) {
            (void)CloseHandle(slot.file);
        }
        slot = {};
    }
}

} // namespace

/** Reads an exact byte range from one package file, keeping the file open. */
bool read(const Path& path, std::uint64_t offset, std::span<std::byte> output) noexcept {
    if (!readable(output)) {
        return false;
    }
    // One lock covers the lookup and the read. The read is positioned on a file the next
    // caller may replace.
    AcquireSRWLockExclusive(&g_lock);
    const HANDLE file = acquire(g_slots, g_useCounter, path);
    const bool complete = file != nullptr && read_positioned(file, offset, output);
    ReleaseSRWLockExclusive(&g_lock);
    return complete;
}

/** Reads an exact byte range through the files one reader keeps open. */
bool read(Scratch& scratch,
          const Path& path,
          std::uint64_t offset,
          std::span<std::byte> output) noexcept {
    if (!readable(output)) {
        return false;
    }
    // No lock: one reader owns this scratch, and no other thread reaches it.
    const HANDLE file = acquire(scratch.files, scratch.slotCounter, path);
    return file != nullptr && read_positioned(file, offset, output);
}

/** Closes the shared files, which the build passes do when they finish. */
void release() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    close_slots(g_slots);
    g_useCounter = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

/** @param scratch Reader whose own files are closed. */
void close(Scratch& scratch) noexcept {
    close_slots(scratch.files);
    scratch.slotCounter = 0;
}

} // namespace sunrise::middleware::content::packages::reader::handle_cache
