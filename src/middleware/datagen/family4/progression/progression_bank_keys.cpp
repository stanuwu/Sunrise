#include "progression_bank_keys.h"

#include <array>

#include "../../../../state/build_data/runtime.h"

namespace sunrise::middleware::datagen::family4::progression {
namespace {

/** Both replicated banks hold 127 rows. */
constexpr std::size_t kBankCapacity = 127;
/** All bits set is the only value the record enumerator treats as an empty slot. */
constexpr std::uint16_t kEmptyDefinitionIndex = 0xFFFF;

} // namespace

/** Keys one object's progression bank from the installed progression definitions. */
bool key_bank(state::build_data::progressions::Scope scope,
              std::span<layout::Entry> bank) noexcept {
    for (layout::Entry& entry : bank) {
        entry.definitionIndex = kEmptyDefinitionIndex;
    }
    std::array<std::uint16_t, kBankCapacity> slots{};
    std::size_t count = 0;
    if (!state::build_data::find_progression_slots(scope, slots, count) || count > bank.size()) {
        return false;
    }
    for (std::size_t slot = 0; slot < count; ++slot) {
        bank[slot].definitionIndex = slots[slot];
    }
    return true;
}

} // namespace sunrise::middleware::datagen::family4::progression
