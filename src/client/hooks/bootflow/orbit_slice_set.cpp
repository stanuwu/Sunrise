#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The slice-set name-hash picker. Anchored on the unset-sentinel store it writes into its output
 * and on the read of the loader context's target field. Together they are unique to it.
 */
constexpr std::string_view kPickerSignatureText = "48 89 5C 24 ? 57 48 83 EC ? 48 8B DA C7 02 C5 "
                                                  "9D 1C 81 48 8B F9 E8 ? ? ? ? 8B 97 04 01 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kPickerSignature =
    signature<signature_length(kPickerSignatureText)>(kPickerSignatureText);

/**
 * Name hash of the orbit slice set, given for the one target the picker cannot find.
 * That target otherwise asks for a hash this build does not carry, so the map load fails and the
 * world is torn down.
 */
constexpr std::uint32_t kOrbitSliceSetHash = 0x8265820B;
/** Target selector the loader carries on the way to orbit. */
constexpr std::uint32_t kOrbitTargetSelector = 0;
/** The picker stores this first. It survives when the registry lookup misses. */
constexpr std::uint32_t kUnresolvedNameHash = 0x811C9DC5;
/** The loader context reserves this many opaque bytes before its target selector. */
constexpr std::size_t kLoaderContextPrefixSize = 0x104;

/** Slice-set loader context, as far as the picker reads it. */
struct LoaderContext {
    std::array<std::byte, kLoaderContextPrefixSize> opaque00{};
    std::uint32_t targetSelector{};
};

static_assert(offsetof(LoaderContext, targetSelector) == kLoaderContextPrefixSize);

/** Picker ABI: loader context in, name hash out, the output returned. */
using PickTarget = std::uint32_t*(__fastcall*)(LoaderContext*, std::uint32_t*) noexcept;

hooking::detour::Handle g_handle{};

/**
 * Supplies the orbit slice set for the one target the picker leaves unset.
 * The original runs first and finds every other target itself. It writes the unset sentinel
 * before it starts, so a surviving sentinel means the registry lookup missed.
 * @param context Loader context carrying the target selector.
 * @param selected Receives the slice-set name hash.
 * @return The output the original returned.
 */
std::uint32_t* __fastcall pick_target(LoaderContext* context, std::uint32_t* selected) noexcept {
    const auto original = reinterpret_cast<PickTarget>(g_handle.original);
    std::uint32_t* const result = original(context, selected);
    if (context != nullptr && selected != nullptr && context->targetSelector == kOrbitTargetSelector
        && *selected == kUnresolvedNameHash) {
        *selected = kOrbitSliceSetHash;
    }
    return result;
}

} // namespace

/** Attaches the picker so the orbit target is found. */
bool install_orbit_slice_set() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const picker = scan_main_image_unique(kPickerSignature, "slice_set_target_picker");
    if (picker == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=slice_set result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{picker, reinterpret_cast<void*>(&pick_target)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=slice_set result=fail reason=attach");
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=slice_set result=ok");
    return true;
}

/** Detaches the picker. */
void uninstall_orbit_slice_set() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
}

} // namespace sunrise::client::hooks::bootflow
