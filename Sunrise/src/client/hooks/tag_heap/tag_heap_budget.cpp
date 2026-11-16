#include "tag_heap_budget.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::tag_heap {
namespace {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

constexpr std::uint64_t kMib = 1024 * 1024;

/**
 * Native tag heap setup. It passes one local to four routines in turn: the first fills the local
 * from the runtime memory settings and the second creates the tag heap from it.
 */
constexpr std::string_view kSetupSignatureText =
    "48 8D 4C 24 20 E8 ? ? ? ? 48 8D 4C 24 20 E8 ? ? ? ? "
    "48 8D 4C 24 20 E8 ? ? ? ? 48 8D 4C 24 20 E8";
constexpr auto kSetupSignature =
    signature<signature_length(kSetupSignatureText)>(kSetupSignatureText);

/**
 * The creating routine storing the new allocator in a global, then setting two of the allocator's
 * fields. The global is null until the heap exists.
 */
constexpr std::string_view kPublishSignatureText =
    "E8 ? ? ? ? 48 89 05 ? ? ? ? 48 8D 0D ? ? ? ? 48 89 48 10 48 89 40 20";
constexpr auto kPublishSignature =
    signature<signature_length(kPublishSignatureText)>(kPublishSignatureText);

/** `lea rax, [rip + settings]` up to its displacement: how the filling routine begins. */
constexpr std::array<std::byte, 3> kLoadSettings{std::byte{0x48}, std::byte{0x8D}, std::byte{0x05}};
/**
 * `push rbx`, `sub rsp, 0x30` and `mov rbx, [rcx + offset]` up to that offset: how the creating
 * routine begins. The offset is where the settings hold the tag heap budget.
 */
constexpr std::array<std::byte, 9> kReadBudget{
    std::byte{0x40},
    std::byte{0x53},
    std::byte{0x48},
    std::byte{0x83},
    std::byte{0xEC},
    std::byte{0x30},
    std::byte{0x48},
    std::byte{0x8B},
    std::byte{0x99},
};

[[nodiscard]] bool write(std::uint64_t* destination, std::uint64_t value) noexcept {
    DWORD protection{};
    if (!VirtualProtect(destination, sizeof value, PAGE_EXECUTE_READWRITE, &protection)) {
        return false;
    }
    *destination = value;
    DWORD ignored{};
    return VirtualProtect(destination, sizeof value, protection, &ignored) != FALSE;
}

/**
 * Stores the budget in the runtime memory settings the tag heap is created from.
 * @param budget New tag heap budget in bytes.
 * @return Null on success, otherwise the short key naming the step that failed.
 */
[[nodiscard]] const char* raise(std::uint64_t budget) noexcept {
    const std::byte* setup = scan_main_image_unique(kSetupSignature, "tag_heap_setup");
    const std::byte* publish = scan_main_image_unique(kPublishSignature, "tag_heap_publish");
    if (!setup || !publish) {
        return "signature";
    }
    // Each pair below is one instruction's RIP displacement and the next instruction it is
    // relative to, counted from the first byte of the match or of the routine.
    const std::byte* fill = resolve_relative(setup + 6, setup + 10);
    const std::byte* create = resolve_relative(setup + 16, setup + 20);
    if (std::memcmp(fill, kLoadSettings.data(), kLoadSettings.size()) != 0
        || std::memcmp(create, kReadBudget.data(), kReadBudget.size()) != 0) {
        return "routine";
    }
    std::uint32_t offset = 0;
    std::memcpy(&offset, create + kReadBudget.size(), sizeof offset);
    auto* field = reinterpret_cast<std::uint64_t*>(resolve_relative(fill + 3, fill + 7) + offset);
    const auto* heap = reinterpret_cast<void* const*>(resolve_relative(publish + 8, publish + 12));
    if (*heap != nullptr) {
        // The client sized the heap already; a later budget would change nothing.
        return "heap_exists";
    }
    if (*field != core::settings::client::kTagHeapStockMib * kMib) {
        return "budget";
    }
    return write(field, budget) ? nullptr : "write";
}

} // namespace

bool install() noexcept {
    const std::uint32_t mib = core::settings::get().client.tagHeapMib;
    if (mib == core::settings::client::kTagHeapStockMib) {
        return true;
    }
    const char* failure = raise(mib * kMib);
    core::log::writef(core::log::Channel::client,
                      failure ? core::log::Level::error : core::log::Level::info,
                      "ev=steam_init stage=tag_heap result=%s mib=%u",
                      failure ? failure : "ok",
                      mib);
    return failure == nullptr;
}

} // namespace sunrise::client::hooks::tag_heap
