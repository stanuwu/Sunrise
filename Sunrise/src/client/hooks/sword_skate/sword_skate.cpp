/**
 * Sword skate. A sword's air attack throws the player forward, and a glide started during the
 * throw keeps that speed. The client refuses the glide while the throw is active.
 * The refusal is one bit of a movement-state field, set by the attack and read by the glide.
 * Clearing it on the tick jump is pressed lets the client start its own glide.
 * Nothing here moves the player.
 */

#include "sword_skate.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::sword_skate {
namespace {

/** Movement-state flags on the player's physics component. */
constexpr std::size_t kMovementStateOffset = 15492;
/**
 * Set while a sword's air attack carries the player, and read by the glide before it starts.
 * The attack sets other bits in the same field. Only this one refuses the glide, so only it is
 * cleared.
 */
constexpr std::uint32_t kGlideRefusedBit = 0x00000800U;
/** The high bit of a polled key state marks it held. */
constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
/** The action this watches. Its binding is the player's jump key. */
constexpr std::uint16_t kJumpAction =
    static_cast<std::uint16_t>(state::account::settings::bindings::Action::jump);

/** Jump held on the previous tick, so the flag is only cleared on the press and not on the hold. */
bool g_jumpHeld{false};
/** Both halves of the jump binding. An unbound half stays empty. */
std::array<std::optional<std::uint16_t>, 2> g_jumpBinding{};
/** Set once the binding is read, so the costly account snapshot runs once. */
bool g_bindingRead{false};

/**
 * Reads one value out of game memory without faulting on a torn pointer.
 * @param address Source address.
 * @param value Receives the value.
 * @return True when Windows copied the whole value.
 */
[[nodiscard]] bool read_at(const std::byte* address, std::uint32_t& value) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

/**
 * Writes one value into game memory. The call applies page protection itself.
 * @param address Destination address.
 * @param value Value to store.
 * @return True when Windows copied the whole value.
 */
[[nodiscard]] bool write_at(std::byte* address, std::uint32_t value) noexcept {
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &written) != FALSE
           && written == sizeof value;
}

/**
 * Takes the account's jump binding once it is loaded.
 * @return True when the binding has been read, whether or not either half is bound.
 */
[[nodiscard]] bool read_jump_binding() noexcept {
    if (g_bindingRead) {
        return true;
    }
    // The snapshot copies the whole account, so it stops once the bindings arrive.
    const state::AccountState account = state::account_snapshot();
    if (!account.settings.keyBindings.configured) {
        return false;
    }
    const auto& binding = account.settings.keyBindings.values[kJumpAction];
    g_jumpBinding = {binding.primary, binding.secondary};
    g_bindingRead = true;
    return true;
}

/**
 * A half is an input code, not a virtual key. A half bound to a mouse button gives no key.
 * @return True while either half of the jump binding is down.
 */
[[nodiscard]] bool jump_down() noexcept {
    for (const std::optional<std::uint16_t>& half : g_jumpBinding) {
        if (!half.has_value()) {
            continue;
        }
        const std::uint32_t key = teleport::action_key(*half);
        if (key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kKeyHeldBit) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

/** Clears the glide refusal for one physics tick of the local player. */
void apply(void* component) noexcept {
    const client::movement::Settings settings = client::movement::get();
    // The interface or another application owns the keyboard. Their presses must not reach this.
    const bool usable = settings.swordSkateEnabled && !core::ui::runtime::snapshot().visible
                        && input::game_focused();
    if (!usable) {
        // Cleared, or the first press after the feature comes back reads as a hold and is skipped.
        g_jumpHeld = false;
        return;
    }
    // Ownership is tested before the key is read. The held flag must only advance on the player's
    // own tick, or another component consumes the press and the player's tick sees no edge.
    if (component == nullptr || !teleport::owns_local_player(component)) {
        return;
    }
    if (!read_jump_binding()) {
        return;
    }
    const bool held = jump_down();
    const bool wasHeld = g_jumpHeld;
    g_jumpHeld = held;
    // Only the press matters. Clearing it for as long as the key is down would hold it clear
    // through the whole attack.
    if (!held || wasHeld) {
        return;
    }
    auto* const bytes = static_cast<std::byte*>(component);
    std::uint32_t state = 0;
    if (!read_at(bytes + kMovementStateOffset, state) || (state & kGlideRefusedBit) == 0) {
        return;
    }
    (void)write_at(bytes + kMovementStateOffset, state & ~kGlideRefusedBit);
}

} // namespace sunrise::client::hooks::sword_skate
