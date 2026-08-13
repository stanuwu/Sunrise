/**
 * Bind for the orbit banner component. Its tick calls the update body only when a dirty byte is
 * set, and an unbound component can never become dirty, so it keeps the constructor's values for
 * the whole run: power zero, no art, emblem state 2. The bind writes the identity triple State
 * already publishes and marks the component dirty once.
 */

#include "banner_bind.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../../state/runtime/runtime.h"
#include "../../hooking/detour.h"

namespace sunrise::client::hooks::banner {
namespace {

using BannerTick = std::int64_t(__fastcall*)(void*, std::int64_t);

hooking::detour::Handle g_handle{};
std::atomic<BannerTick> g_original{nullptr};
std::atomic<unsigned> g_reported{0};

/** What the update body left in the component, read after it has run. */
struct Published {
    std::int32_t light{};
    std::uint16_t definition{};
    std::uint32_t art{};
    std::uint32_t state{};
};

/** One resolved identity the banner can be bound to. */
struct Identity {
    std::uint64_t account{};
    std::uint64_t character{};
    bool resolved{};
};

/** @return The account and selected-character keys State publishes, when both are set. */
[[nodiscard]] Identity selected_identity() noexcept {
    const state::AccountState account = state::account_snapshot();
    if (account.primarySoid == 0) {
        return Identity{};
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const state::CharacterState& character = account.characters[index];
        if (character.selected && character.soid != 0) {
            return Identity{account.primarySoid, character.soid, true};
        }
    }
    return Identity{};
}

/**
 * Reports one bind and what the update body then published, so a boot says whether the bind was
 * enough, not just that it happened.
 * @param identity Keys the component was bound to.
 * @param self Component address, which separates one instance from another.
 * @param published What the update body wrote once it had run.
 */
void report(const Identity& identity, const void* self, const Published& published) noexcept {
    const unsigned emitted = g_reported.load(std::memory_order_relaxed);
    if (emitted >= kMaxBindReports) {
        return;
    }
    g_reported.store(emitted + 1, std::memory_order_relaxed);
    std::array<char, kReportCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=banner stage=bind this=0x%016llX character=0x%016llX"
                                      " light=%d emblem_def=%u emblem_art=0x%08X emblem_state=%u"
                                      " result=bound",
                                      reinterpret_cast<unsigned long long>(self),
                                      static_cast<unsigned long long>(identity.character),
                                      static_cast<int>(published.light),
                                      static_cast<unsigned>(published.definition),
                                      static_cast<unsigned>(published.art),
                                      static_cast<unsigned>(published.state));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Binds an unbound component before its tick reads the triple, then runs the original.
 * @param self Borrowed banner component.
 * @param frame Frame argument the tick forwards unchanged.
 * @return The tick's own result.
 */
__declspec(noinline) std::int64_t __fastcall tick(void* self, std::int64_t frame) noexcept {
    const BannerTick original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return 0;
    }
    if (self == nullptr) {
        return original(self, frame);
    }
    std::byte* const component = static_cast<std::byte*>(self);
    std::uint64_t account = 0;
    std::uint8_t family = 0;
    std::int32_t currentLight = 0;
    std::uint32_t emblemState = 0;
    std::memcpy(&account, component + BannerLayout::accountKey, sizeof account);
    std::memcpy(&family, component + BannerLayout::family, sizeof family);
    std::memcpy(&currentLight, component + BannerLayout::light, sizeof currentLight);
    std::memcpy(&emblemState, component + BannerLayout::emblemState, sizeof emblemState);
    // A component can bind before the Family-4 snapshot is usable. In that case the update body
    // leaves power at zero or emblem state 2. Keep retrying that incomplete component; once both
    // values resolve, the ordinary clean fast path resumes.
    if (account != 0 && family != kBannerFamilyNone && currentLight > 0 && emblemState == 0) {
        // The presentation widget is a second consumer behind this component. A one-shot dirty
        // transition updates the component but can occur before that widget subscribes, leaving
        // the visible card at its constructor defaults. Keep the resolved component publishable
        // while it is ticking so late-created orbit/fireteam views receive the same values.
        std::memcpy(component + BannerLayout::dirty, &kBannerDirty, sizeof kBannerDirty);
        return original(self, frame);
    }
    const Identity identity = selected_identity();
    if (!identity.resolved) {
        return original(self, frame);
    }
    std::memcpy(component + BannerLayout::accountKey, &identity.account, sizeof identity.account);
    std::memcpy(
        component + BannerLayout::characterKey, &identity.character, sizeof identity.character);
    std::memcpy(component + BannerLayout::family, &kBannerFamily, sizeof kBannerFamily);
    std::memcpy(component + BannerLayout::dirty, &kBannerDirty, sizeof kBannerDirty);
    // The update body runs inside this call, so its published fields are read after it returns.
    const std::int64_t result = original(self, frame);
    Published published{};
    std::memcpy(&published.light, component + BannerLayout::light, sizeof published.light);
    std::memcpy(&published.definition,
                component + BannerLayout::emblemDefinition,
                sizeof published.definition);
    std::memcpy(&published.art, component + BannerLayout::emblemArt, sizeof published.art);
    std::memcpy(&published.state, component + BannerLayout::emblemState, sizeof published.state);
    report(identity, self, published);
    return result;
}

/** @param reason Key naming the step that failed. @return False, for a direct return. */
bool fail_install(const char* reason) noexcept {
    std::array<char, kReportCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=banner stage=bind result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Finds the banner tick and attaches the bind. */
bool install_banner_bind() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kBannerTickSignature, "banner_tick");
    if (target == nullptr) {
        return fail_install("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&tick)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail_install("attach");
    }
    g_original.store(reinterpret_cast<BannerTick>(g_handle.original), std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=banner stage=bind result=ok");
    return true;
}

/** Detaches the banner bind and drops the trampoline it got. */
void uninstall_banner_bind() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reported.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::banner
