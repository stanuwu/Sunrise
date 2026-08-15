#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::banner {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/** The orbit banner component's per-frame tick, matched on its prologue and argument moves. */
inline constexpr std::string_view kBannerTickSignatureText =
    "48 89 5C 24 ? 55 56 57 41 55 41 56 48 83 EC ? 4C 8B F2 48 8B D9";
/** Compiled pattern bytes of the signature text above. */
inline constexpr auto kBannerTickSignature =
    signature<signature_length(kBannerTickSignatureText)>(kBannerTickSignatureText);

/**
 * Fields of the banner component the bind writes, as byte offsets from its base. The tick calls
 * its update body only when a dirty byte is set, and an unbound component never becomes dirty.
 */
struct BannerLayout {
    /** Account key of the identity triple the update body looks up. */
    static constexpr std::size_t accountKey = 136;
    /** Character key of the same triple. */
    static constexpr std::size_t characterKey = 144;
    /** Queuez family the triple is resolved against, read as a signed byte. */
    static constexpr std::size_t family = 152;
    /** Dirty byte the tick tests and then clears. */
    static constexpr std::size_t dirty = 1196;
    /** Light the update body publishes. Read only, to report whether the bind produced one. */
    static constexpr std::size_t light = 548;
    /**
     * Emblem fields the update body publishes, as `a3 + 492/504/512` where `a3` is this base plus
     * 160: the item definition index, the resolved art entry, and the state. State 2 is the
     * unresolved branch, which writes -1 into both of the others.
     */
    static constexpr std::size_t emblemDefinition = 652;
    /** Resolved art entry, written into both halves of a qword. */
    static constexpr std::size_t emblemArt = 664;
    /** Emblem state, 0 when resolved and 2 when the update body found no item. */
    static constexpr std::size_t emblemState = 672;
};

/**
 * Family 4 carries the objects whose banners do update, and it picks the update arm that finds
 * both the emblem and the light. The registrar refuses anything above 7.
 */
inline constexpr std::uint8_t kBannerFamily = 4;
/** Family value of an unbound component. It is read signed, so it sign-extends to -1. */
inline constexpr std::uint8_t kBannerFamilyNone = 0xFF;
/** Value written into the dirty byte so the tick runs its update body once. */
inline constexpr std::uint8_t kBannerDirty = 1;

/** Bind lines allowed per run. The tick runs every frame, so an uncapped report buries the log. */
inline constexpr unsigned kMaxBindReports = 12;

/** Bounds one report line at the width of its fields. */
inline constexpr std::size_t kReportCapacity = 224;

/**
 * Attaches the banner bind, which gives the orbit banner component an identity so its update body
 * runs. Without it the component keeps its constructor values: power zero, no art, emblem state 2.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_banner_bind() noexcept;

/** Detaches the banner bind. */
void uninstall_banner_bind() noexcept;

} // namespace sunrise::client::hooks::banner
