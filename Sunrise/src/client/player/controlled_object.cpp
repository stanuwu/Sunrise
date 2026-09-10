#include "controlled_object.h"

#include <atomic>
#include <string_view>

#include "../../core/logging/log.h"
#include "../patterns/image_scan.h"

namespace sunrise::client::player::controlled_object {
namespace {

constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFFU;

/** Keeps the native function type used by the original teleport implementation. */
using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);

constexpr std::string_view kControlledHandleText =
    "40 53 48 83 EC 20 48 8B D9 C7 01 FF FF FF FF 48 8D 4C 24 30 E8 ? ? ? ? 8B 44 24 30 "
    "83 F8 FF 74 18 25 FF 1F 00 00 0F AF 05";
constexpr auto kControlledHandlePattern =
    patterns::signature<patterns::signature_length(kControlledHandleText)>(kControlledHandleText);

std::atomic<ControlledHandle> g_accessor{nullptr};

} // namespace

bool resolve() noexcept {
    if (available()) {
        return true;
    }

    std::byte* const address =
        patterns::scan_main_image_unique(kControlledHandlePattern, "controlled_object_handle");

    if (address == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=controlled_object stage=resolve result=fail reason=signature");
        return false;
    }

    g_accessor.store(reinterpret_cast<ControlledHandle>(address), std::memory_order_release);
    return true;
}

bool available() noexcept {
    return g_accessor.load(std::memory_order_acquire) != nullptr;
}

bool current_handle(std::uint32_t& handle) noexcept {
    handle = kInvalidHandle;

    const ControlledHandle accessor = g_accessor.load(std::memory_order_acquire);
    if (accessor == nullptr) {
        return false;
    }

    std::uint32_t candidate = kInvalidHandle;
    if (accessor(&candidate) == nullptr || candidate == kInvalidHandle) {
        return false;
    }

    handle = candidate;
    return true;
}

void clear() noexcept {
    g_accessor.store(nullptr, std::memory_order_release);
}

} // namespace sunrise::client::player::controlled_object