#include "internal.h"

namespace sunrise::steam::interfaces::tables {
namespace {

INIT_ONCE g_interfaceInit{INIT_ONCE_STATIC_INIT};

/**
 * Sets up every interface table before any object is published.
 * @return TRUE for InitOnceExecuteOnce.
 */
BOOL CALLBACK initialize_tables(PINIT_ONCE, PVOID, PVOID*) noexcept {
    initialize_common();
    initialize_networking();
    return TRUE;
}

} // namespace

/** @return True once every interface table is set up. */
bool initialize() noexcept {
    return InitOnceExecuteOnce(&g_interfaceInit, initialize_tables, nullptr, nullptr) != FALSE;
}

} // namespace sunrise::steam::interfaces::tables
