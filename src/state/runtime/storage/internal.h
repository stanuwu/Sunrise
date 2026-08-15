#pragma once

#include <Windows.h>

#include "../state.h"

namespace sunrise::state::runtime::storage {

/** Root process-local State shared by folder-backed State submodules. */
extern State g_state;
/** Windows reader-writer lock protecting mutable root State fields. */
extern SRWLOCK g_stateLock;

} // namespace sunrise::state::runtime::storage
