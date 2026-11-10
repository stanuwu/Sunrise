#pragma once

#include <span>

#include "../../../../state/build_data/progressions/definition.h"
#include "../../../../state/unlocks/definition.h"
#include "layout.h"

namespace sunrise::middleware::datagen::family4::progression {

/**
 * Keys one object's progression bank and fills each keyed row from the authored lanes.
 * Slots start at the empty definition index: 0 is a real progression, and the record enumerator
 * skips a slot only when all bits are set. Keyed slots are this object's scope, in native order.
 * @param scope Replicated object owning the bank.
 * @param bank Fixed progression rows to key.
 * @param unlocks Live or prepared unlock snapshot from the same view as the encoded object.
 * @return True when the definition table is ready and every keyed slot fits the bank.
 */
[[nodiscard]] bool key_bank(state::build_data::progressions::Scope scope,
                            std::span<layout::Entry> bank,
                            const state::unlocks::Table& unlocks) noexcept;

} // namespace sunrise::middleware::datagen::family4::progression
