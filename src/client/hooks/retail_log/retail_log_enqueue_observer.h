#pragma once

#include <Windows.h>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::retail_log {

extern SRWLOCK g_lock;
extern hooking::detour::Handle g_handle;

/** @return The enqueue observer body itself, with internal linkage. */
[[nodiscard]] void* enqueue_entry_point() noexcept;

/** Applies the configured category threshold, once we know the log block exists. */
void assert_verbosity() noexcept;

} // namespace sunrise::client::hooks::retail_log
