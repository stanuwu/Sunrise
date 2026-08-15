#include "game_package_targets.h"

#include <array>

#include "../../../memory/current_process_memory.h"
#include "../../../patterns/game/packages/package_key_signature_bytes.h"
#include "../relative.h"

namespace sunrise::client::targets::game::packages {
namespace {

/** The table address is the signed displacement of the matched vector load. */
constexpr std::size_t kTableDisplacementOffset = 3;
/** That load ends after its 4-byte displacement. */
constexpr std::size_t kTableInstructionEndOffset = 7;

Targets g_targets;
bool g_resolved{};

} // namespace

/** Derives the package key-table location. */
bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept {
    output = {};
    const patterns::Pattern pattern{"package_key_table", patterns::game::packages::kKeyTable};
    std::array<patterns::Match, 1> match{};
    if (!patterns::resolve_all(image, std::span(&pattern, 1), match)
        || match[0].status != patterns::MatchStatus::unique) {
        return false;
    }
    Targets resolved;
    if (!relative::resolve(match[0].address,
                           kTableDisplacementOffset,
                           kTableInstructionEndOffset,
                           resolved.keyTable)) {
        return false;
    }
    output = resolved;
    return true;
}

/** @param targets Validated package table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
    g_resolved = true;
}

/** Clears the published package target group. */
void clear() noexcept {
    g_targets = {};
    g_resolved = false;
}

/** @return Process-local package target group. */
const Targets& get() noexcept {
    return g_targets;
}

/** @return True after the key table is published. */
bool is_resolved() noexcept {
    return g_resolved;
}

/** Copies the installed key table for one bounded use. */
bool read(KeyTable& table) noexcept {
    table = {};
    if (!g_resolved || g_targets.keyTable == nullptr) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(g_targets.keyTable);
    return memory::read_current_process(
        nullptr, base, std::as_writable_bytes(std::span{&table, 1}));
}

} // namespace sunrise::client::targets::game::packages
