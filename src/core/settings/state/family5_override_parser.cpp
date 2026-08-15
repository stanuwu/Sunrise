#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** The family-5 flag encoder rejects any override slot above 23499. */
constexpr std::uint64_t kMaximumFlagSlot = 23499;
/** Flag values run 0 to 2. The family-5 encoder adds the wire bias. */
constexpr std::uint64_t kMaximumFlagValue = 2;
/** The family-5 value encoder rejects any override slot above 15499. */
constexpr std::uint64_t kMaximumValueSlot = 15499;
/** Both family-5 override counts ship in a 7-bit wire field, so 127 rows is the wire limit. */
constexpr std::size_t kMaximumOverrideRows = 127;

// Fixed override storage is the tighter bound, so row parsers only check the array size.
static_assert(state::kUnlockOverrideCapacity <= kMaximumOverrideRows,
              "Override storage must fit the seven-bit wire count field.");

} // namespace

/** Parses the optional authored family-5 override group. */
bool Parser::investment(state::Family5State& output) noexcept {
    state::Family5State parsed{};
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        output = parsed;
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        bool complete = false;
        if (key == "family5_flag_overrides") {
            complete = unlock_flag_overrides(parsed);
        } else if (key == "family5_value_overrides") {
            complete = unlock_value_overrides(parsed);
        } else {
            complete = skip_value(0);
        }
        if (!complete) {
            return false;
        }
        if (consume('}')) {
            // Publish both lists together, so a refused row leaves State untouched.
            output = parsed;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Fills the flag-override list from [slot, value] pairs. */
bool Parser::unlock_flag_overrides(state::Family5State& output) noexcept {
    output.flagCount = 0;
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        std::uint64_t slot = 0;
        std::uint64_t value = 0;
        if (output.flagCount >= output.flags.size() || !consume('[') || !unsigned_integer(slot)
            || !consume(',') || !unsigned_integer(value) || !consume(']') || slot > kMaximumFlagSlot
            || value > kMaximumFlagValue) {
            return false;
        }
        state::UnlockFlagOverride& row = output.flags[output.flagCount++];
        row.slot = static_cast<std::uint16_t>(slot);
        row.value = static_cast<std::uint8_t>(value);
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Fills the signed value-override list from [slot, value] pairs. */
bool Parser::unlock_value_overrides(state::Family5State& output) noexcept {
    output.valueCount = 0;
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        std::uint64_t slot = 0;
        std::int32_t value = 0;
        if (output.valueCount >= output.values.size() || !consume('[') || !unsigned_integer(slot)
            || !consume(',') || !signed_32(value) || !consume(']') || slot > kMaximumValueSlot) {
            return false;
        }
        state::UnlockValueOverride& row = output.values[output.valueCount++];
        row.slot = static_cast<std::uint16_t>(slot);
        row.value = value;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
