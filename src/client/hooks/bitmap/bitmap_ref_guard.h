#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::bitmap {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * The bitmap-reference setter. Seven sibling widget classes share its first 16 bytes and differ
 * only in the stored-field offset, so the signature must reach the `+0x128` compare.
 */
inline constexpr std::string_view kSetterSignatureText =
    "40 53 48 83 EC ? F2 0F 10 02 48 8B D9 8B 4A 08 "
    "F2 0F 11 44 24 ? 8B 44 24 ? 39 83 28 01 00 00";
/** Compiled pattern bytes of the signature text above. */
inline constexpr auto kSetterSignature =
    signature<signature_length(kSetterSignatureText)>(kSetterSignatureText);

/** Words of the 12-byte reference triple the setter writes. */
struct BitmapRefWords {
    /** Definition tag of the bound property. */
    static constexpr std::size_t tag = 0;
    /** The bitmap reference itself. Only this word is guarded. */
    static constexpr std::size_t reference = 1;
    /** Draw state the widget stores beside the reference. */
    static constexpr std::size_t state = 2;
    /** Words in one triple. */
    static constexpr std::size_t count = 3;
};

/** Reference meaning no bitmap. The reader skips its decode, so it can never fault. */
inline constexpr std::uint32_t kNoneReference = 0xFFFFFFFFu;

/**
 * Tag space is the whole `0x80` and `0x81` top byte. A narrower test blanks real bitmaps:
 * `0x80A146C7` and its neighbours are real. A zero or a definition hash sits below `0x80`.
 */
inline constexpr std::uint32_t kTagTopByteLow = 0x80u;
/** Upper half of tag space. */
inline constexpr std::uint32_t kTagTopByteHigh = 0x81u;
/** Aligns a reference's top byte down for the tag test. */
inline constexpr int kTopByteShift = 24;

/**
 * Guard lines allowed per run. A rejected reference repeats on every widget refresh, so an
 * uncapped guard buries the rest of the log. The first few carry the reference and its tag.
 */
inline constexpr unsigned kMaxReports = 8;

/** Size of one guard line, set by its reference, tag and state fields. */
inline constexpr std::size_t kReportCapacity = 128;

/**
 * New references reported per run while watching is on. It answers one question: does a bitmap
 * ever reach a widget. Repeats are dropped, so the cap counts distinct references.
 */
inline constexpr unsigned kMaxSeenReports = 40;

/**
 * Attaches the bitmap-reference guard. It keeps a reference that is not a tag out of the widget
 * field a later reader decodes.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_bitmap_ref_guard() noexcept;

/** Detaches the bitmap-reference guard. */
void uninstall_bitmap_ref_guard() noexcept;

} // namespace sunrise::client::hooks::bitmap
