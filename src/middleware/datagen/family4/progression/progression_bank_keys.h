#pragma once

#include <span>

#include "../../../../state/build_data/progressions/definition.h"
#include "layout.h"

namespace sunrise::middleware::datagen::family4::progression {

/**
 * Keys one object's progression bank from the installed progression definitions.
 * Slots start at the empty definition index: 0 is a real progression, and the record enumerator
 * skips a slot only when all bits are set. Keyed slots are this object's scope, in native order.
 * @param scope Replicated object owning the bank.
 * @param bank Fixed progression rows to key.
 * @return True when the definition table is ready and every keyed slot fits the bank.
 */
[[nodiscard]] bool key_bank(state::build_data::progressions::Scope scope,
                            std::span<layout::Entry> bank) noexcept;

} // namespace sunrise::middleware::datagen::family4::progression
