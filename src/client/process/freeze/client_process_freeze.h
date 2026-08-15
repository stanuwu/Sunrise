#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>

namespace sunrise::client::process::freeze {

/** 1024 threads cover the game's pools without heap storage. */
inline constexpr std::size_t kThreadLimit = 1024;

/** Threads one hold keeps suspended, and the process locks it owns with them. */
struct Held {
    std::array<HANDLE, kThreadLimit> handles{};
    std::array<DWORD, kThreadLimit> ids{};
    std::size_t count{};
    ULONG_PTR loaderCookie{};
    bool loaderLocked{};
    bool heapLocked{};
};

/**
 * Suspends every other thread in the process until the matching release.
 * The game cannot time a connection out while its threads do not run, so slow work here no
 * longer drops the session. Held threads never own a lock this thread takes next.
 * @param held Receives the handles the release must resume.
 * @return True when the process is held. False leaves every thread running.
 */
[[nodiscard]] bool hold(Held& held) noexcept;

/**
 * Resumes and releases every held thread. Call on the thread that held them.
 * @param held Handles from a hold. An empty set is accepted.
 */
void release(Held& held) noexcept;

/**
 * Blocks until this thread is the only suspender in the process.
 * Detours transactions suspend threads too. Two suspenders running at once stop each other,
 * because each waits on a thread the other froze.
 */
void enter_exclusive() noexcept;

/** Releases the claim taken by enter_exclusive. */
void leave_exclusive() noexcept;

} // namespace sunrise::client::process::freeze
