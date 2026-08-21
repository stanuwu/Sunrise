#include "bootflow_texture_override.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "../../../../resources/resource.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::bootflow::texture_override {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Resourcerer's GPU-entry dispatcher. Its second argument is the TagHash and its third and fourth
 * arguments are the decoded entry pointer and byte count. The switch immediately after this
 * prologue distinguishes the GPU resource classes.
 */
constexpr std::string_view kGpuEntryDispatcherText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
    "49 8B F9 49 8B F0 8B DA 8B E9 E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? "
    "8D 45 FF 83 F8 12";
constexpr auto kGpuEntryDispatcher =
    signature<signature_length(kGpuEntryDispatcherText)>(kGpuEntryDispatcherText);

constexpr std::size_t kTigerTextureHeaderSize = 0x28;
constexpr std::size_t kDdsLegacyHeaderSize = 4 + 124;
constexpr std::size_t kDdsDx10HeaderSize = kDdsLegacyHeaderSize + 20;
constexpr std::uint32_t kDdsMagic = 0x20534444U;
constexpr std::uint32_t kDx10FourCc = 0x30315844U;
constexpr std::uint32_t kDdsPixelAlphaPixels = 0x1U;
constexpr std::uint32_t kDdsPixelAlpha = 0x2U;
constexpr std::uint32_t kDdsPixelFourCc = 0x4U;
constexpr std::uint32_t kDdsPixelRgb = 0x40U;
constexpr std::uint32_t kDdsPixelLuminance = 0x20000U;
constexpr std::uint32_t kDdsResourceTexture2d = 3U;
constexpr std::uint32_t kDdsResourceMiscCube = 0x4U;
constexpr std::uint16_t kTigerTextureMarker = 0xCAFEU;
constexpr std::uint32_t kGpuTextureClass = 1U;

constexpr std::size_t kTigerDataSizeOffset = 0x00;
constexpr std::size_t kTigerFormatOffset = 0x04;
constexpr std::size_t kTigerMarkerOffset = 0x0C;
constexpr std::size_t kTigerWidthOffset = 0x0E;
constexpr std::size_t kTigerHeightOffset = 0x10;
constexpr std::size_t kTigerDepthOffset = 0x12;
constexpr std::size_t kTigerArraySizeOffset = 0x14;

/** Exact stock texture-header/data pairs selected from package 0x010A. */
struct AssetSpec final {
    std::uint32_t headerTag{};
    std::uint32_t dataTag{};
    int resourceId{};
};

constexpr std::array kAssetSpecs{
    AssetSpec{0x80A145FEU, 0x80A145FFU, IDR_BOOTFLOW_TEXTURE_80A145FF},
    AssetSpec{0x80A14602U, 0x80A14601U, IDR_BOOTFLOW_TEXTURE_80A14601},
    AssetSpec{0x80A14608U, 0x80A14607U, IDR_BOOTFLOW_TEXTURE_80A14607},
    AssetSpec{0x80A1460DU, 0x80A1460EU, IDR_BOOTFLOW_TEXTURE_80A1460E},
    AssetSpec{0x80A1461CU, 0x80A1461DU, IDR_BOOTFLOW_TEXTURE_80A1461D},
    AssetSpec{0x80A14620U, 0x80A1461FU, IDR_BOOTFLOW_TEXTURE_80A1461F},
    AssetSpec{0x80A14622U, 0x80A14621U, IDR_BOOTFLOW_TEXTURE_80A14621},
    AssetSpec{0x80A14623U, 0x80A14624U, IDR_BOOTFLOW_TEXTURE_80A14624},
    AssetSpec{0x80A14626U, 0x80A14625U, IDR_BOOTFLOW_TEXTURE_80A14625},
    AssetSpec{0x80A14627U, 0x80A14628U, IDR_BOOTFLOW_TEXTURE_80A14628},
    AssetSpec{0x80A1462AU, 0x80A14629U, IDR_BOOTFLOW_TEXTURE_80A14629},
    AssetSpec{0x80A1462CU, 0x80A1462BU, IDR_BOOTFLOW_TEXTURE_80A1462B},
    AssetSpec{0x80A1462DU, 0x80A1462EU, IDR_BOOTFLOW_TEXTURE_80A1462E},
    AssetSpec{0x80A14630U, 0x80A1462FU, IDR_BOOTFLOW_TEXTURE_80A1462F},
    AssetSpec{0x80A14632U, 0x80A14631U, IDR_BOOTFLOW_TEXTURE_80A14631},
    AssetSpec{0x80A14634U, 0x80A14633U, IDR_BOOTFLOW_TEXTURE_80A14633},
    AssetSpec{0x80A14635U, 0x80A14636U, IDR_BOOTFLOW_TEXTURE_80A14636},
    AssetSpec{0x80A146D4U, 0x80A146D5U, IDR_BOOTFLOW_TEXTURE_80A146D5},
};

struct DdsView final {
    const std::byte* pixels{};
    std::uint32_t pixelSize{};
    std::uint32_t format{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t depth{};
    std::uint16_t arraySize{};
};

struct Asset final {
    AssetSpec spec{};
    DdsView dds{};
    std::array<std::byte, kTigerTextureHeaderSize> header{};
    bool headerReady{};
    bool reported{};
};

using GpuEntryDispatcher = std::uint64_t(__fastcall*)(std::uint32_t resourceClass,
                                                      std::uint32_t tag,
                                                      const void* decoded,
                                                      std::uint64_t decodedSize) noexcept;

hooking::detour::Handle g_handle{};
SRWLOCK g_assetLock{SRWLOCK_INIT};
std::array<Asset, kAssetSpecs.size()> g_assets{};

template <typename Value>
[[nodiscard]] bool
load_value(const std::byte* bytes, std::size_t size, std::size_t offset, Value& output) noexcept {
    if (bytes == nullptr || offset > size || sizeof(Value) > size - offset) {
        return false;
    }
    std::memcpy(&output, bytes + offset, sizeof output);
    return true;
}

template <typename Value>
void store_value(std::span<std::byte> bytes, std::size_t offset, Value value) noexcept {
    if (offset <= bytes.size() && sizeof(Value) <= bytes.size() - offset) {
        std::memcpy(bytes.data() + offset, &value, sizeof value);
    }
}

[[nodiscard]] constexpr std::uint32_t
four_cc(char first, char second, char third, char fourth) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(first))
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(second)) << 8U)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(third)) << 16U)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(fourth)) << 24U);
}

/** Converts the legacy DDS formats used by the supplied bootflow assets to DXGI values. */
[[nodiscard]] bool
legacy_format(const std::byte* bytes, std::size_t size, std::uint32_t& output) noexcept {
    std::uint32_t flags = 0;
    std::uint32_t formatFourCc = 0;
    std::uint32_t bits = 0;
    std::uint32_t red = 0;
    std::uint32_t green = 0;
    std::uint32_t blue = 0;
    std::uint32_t alpha = 0;
    if (!load_value(bytes, size, 0x50, flags) || !load_value(bytes, size, 0x54, formatFourCc)
        || !load_value(bytes, size, 0x58, bits) || !load_value(bytes, size, 0x5C, red)
        || !load_value(bytes, size, 0x60, green) || !load_value(bytes, size, 0x64, blue)
        || !load_value(bytes, size, 0x68, alpha)) {
        return false;
    }
    if ((flags & kDdsPixelFourCc) != 0) {
        switch (formatFourCc) {
        case four_cc('D', 'X', 'T', '1'):
            output = 71U;
            return true;
        case four_cc('D', 'X', 'T', '3'):
            output = 74U;
            return true;
        case four_cc('D', 'X', 'T', '5'):
            output = 77U;
            return true;
        case four_cc('A', 'T', 'I', '1'):
        case four_cc('B', 'C', '4', 'U'):
            output = 80U;
            return true;
        case four_cc('A', 'T', 'I', '2'):
        case four_cc('B', 'C', '5', 'U'):
            output = 83U;
            return true;
        default:
            return false;
        }
    }
    if ((flags & kDdsPixelRgb) != 0 && bits == 32U) {
        if (red == 0x000000FFU && green == 0x0000FF00U && blue == 0x00FF0000U
            && alpha == 0xFF000000U) {
            output = 28U;
            return true;
        }
        if (red == 0x00FF0000U && green == 0x0000FF00U && blue == 0x000000FFU) {
            output = alpha == 0xFF000000U ? 87U : 88U;
            return alpha == 0xFF000000U || alpha == 0U;
        }
    }
    if ((flags & kDdsPixelRgb) != 0 && bits == 16U) {
        if (red == 0xF800U && green == 0x07E0U && blue == 0x001FU && alpha == 0U) {
            output = 85U;
            return true;
        }
        if (red == 0x7C00U && green == 0x03E0U && blue == 0x001FU && alpha == 0x8000U) {
            output = 86U;
            return true;
        }
        if (red == 0x0F00U && green == 0x00F0U && blue == 0x000FU && alpha == 0xF000U) {
            output = 115U;
            return true;
        }
    }
    if ((flags & kDdsPixelLuminance) != 0 && bits == 8U && red == 0xFFU) {
        output = 61U;
        return true;
    }
    if ((flags & kDdsPixelLuminance) != 0 && (flags & kDdsPixelAlphaPixels) != 0 && bits == 16U
        && red == 0x00FFU && alpha == 0xFF00U) {
        output = 49U;
        return true;
    }
    if ((flags & kDdsPixelAlpha) != 0 && bits == 8U && alpha == 0xFFU) {
        output = 65U;
        return true;
    }
    return false;
}

/** Parses one embedded 2D DDS without allocating or copying its pixel payload. */
[[nodiscard]] bool parse_dds(const std::byte* bytes, std::size_t size, DdsView& output) noexcept {
    output = {};
    std::uint32_t magic = 0;
    std::uint32_t headerSize = 0;
    std::uint32_t pixelHeaderSize = 0;
    std::uint32_t pixelFlags = 0;
    std::uint32_t formatFourCc = 0;
    if (size < kDdsLegacyHeaderSize || !load_value(bytes, size, 0x00, magic)
        || !load_value(bytes, size, 0x04, headerSize)
        || !load_value(bytes, size, 0x4C, pixelHeaderSize)
        || !load_value(bytes, size, 0x50, pixelFlags)
        || !load_value(bytes, size, 0x54, formatFourCc) || magic != kDdsMagic || headerSize != 124U
        || pixelHeaderSize != 32U) {
        return false;
    }
    const bool dx10 = (pixelFlags & kDdsPixelFourCc) != 0 && formatFourCc == kDx10FourCc;
    const std::size_t pixelOffset = dx10 ? kDdsDx10HeaderSize : kDdsLegacyHeaderSize;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t arraySize = 1;
    std::uint32_t resourceDimension = kDdsResourceTexture2d;
    std::uint32_t miscFlag = 0;
    std::uint32_t format = 0;
    if (size <= pixelOffset || !load_value(bytes, size, 0x10, width)
        || !load_value(bytes, size, 0x0C, height) || !load_value(bytes, size, 0x18, depth)) {
        return false;
    }
    if (dx10) {
        if (!load_value(bytes, size, 0x80, format)
            || !load_value(bytes, size, 0x84, resourceDimension)
            || !load_value(bytes, size, 0x88, miscFlag)
            || !load_value(bytes, size, 0x8C, arraySize)) {
            return false;
        }
    } else if (!legacy_format(bytes, size, format)) {
        return false;
    }
    const std::size_t pixelSize = size - pixelOffset;
    if (width == 0 || height == 0 || width > 0xFFFFU || height > 0xFFFFU || pixelSize == 0
        || pixelSize > 0xFFFFFFFFULL || format == 0 || resourceDimension != kDdsResourceTexture2d
        || (miscFlag & kDdsResourceMiscCube) != 0 || arraySize == 0 || arraySize > 0xFFFFU) {
        return false;
    }
    output = DdsView{bytes + pixelOffset,
                     static_cast<std::uint32_t>(pixelSize),
                     format,
                     static_cast<std::uint16_t>(width),
                     static_cast<std::uint16_t>(height),
                     1,
                     static_cast<std::uint16_t>(arraySize)};
    return true;
}

/** Loads and validates every embedded DDS before the detour can expose any of them. */
[[nodiscard]] bool load_assets(HMODULE module) noexcept {
    if (module == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < kAssetSpecs.size(); ++index) {
        const AssetSpec& spec = kAssetSpecs[index];
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(spec.resourceId), RT_RCDATA);
        if (resource == nullptr) {
            return false;
        }
        const HGLOBAL loaded = LoadResource(module, resource);
        const DWORD size = SizeofResource(module, resource);
        const auto* bytes = static_cast<const std::byte*>(LockResource(loaded));
        Asset asset{};
        asset.spec = spec;
        if (loaded == nullptr || bytes == nullptr || size == 0
            || !parse_dds(bytes, static_cast<std::size_t>(size), asset.dds)) {
            return false;
        }
        g_assets[index] = asset;
    }
    return true;
}

/** @return The asset owning this exact header or data TagHash. */
[[nodiscard]] Asset* find_asset(std::uint32_t tag, bool& header) noexcept {
    for (Asset& asset : g_assets) {
        if (asset.spec.headerTag == tag) {
            header = true;
            return &asset;
        }
        if (asset.spec.dataTag == tag) {
            header = false;
            return &asset;
        }
    }
    return nullptr;
}

/** Builds one replacement Tiger descriptor from its stock descriptor and embedded DDS. */
[[nodiscard]] const void*
prepare_header(Asset& asset, const void* stock, std::uint64_t stockSize, bool& report) noexcept {
    report = false;
    if (stock == nullptr || stockSize < kTigerTextureHeaderSize) {
        return stock;
    }
    AcquireSRWLockExclusive(&g_assetLock);
    if (!asset.headerReady) {
        std::memcpy(asset.header.data(), stock, asset.header.size());
        std::uint16_t marker = 0;
        std::memcpy(&marker, asset.header.data() + kTigerMarkerOffset, sizeof marker);
        if (marker == kTigerTextureMarker) {
            const std::span header(asset.header);
            store_value(header, kTigerDataSizeOffset, asset.dds.pixelSize);
            store_value(header, kTigerFormatOffset, asset.dds.format);
            store_value(header, kTigerWidthOffset, asset.dds.width);
            store_value(header, kTigerHeightOffset, asset.dds.height);
            store_value(header, kTigerDepthOffset, asset.dds.depth);
            store_value(header, kTigerArraySizeOffset, asset.dds.arraySize);
            asset.headerReady = true;
        }
    }
    if (asset.headerReady && !asset.reported) {
        asset.reported = true;
        report = true;
    }
    const void* result = asset.headerReady ? asset.header.data() : stock;
    ReleaseSRWLockExclusive(&g_assetLock);
    return result;
}

void report_override(const Asset& asset) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow_texture stage=entry tag=0x%08X size=%u "
                                      "width=%u height=%u result=override",
                                      static_cast<unsigned>(asset.spec.dataTag),
                                      static_cast<unsigned>(asset.dds.pixelSize),
                                      static_cast<unsigned>(asset.dds.width),
                                      static_cast<unsigned>(asset.dds.height));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

/** Replaces only selected decoded GPU texture entries, then preserves the native dispatcher. */
std::uint64_t __fastcall dispatch(std::uint32_t resourceClass,
                                  std::uint32_t tag,
                                  const void* decoded,
                                  std::uint64_t decodedSize) noexcept {
    const auto original = reinterpret_cast<GpuEntryDispatcher>(g_handle.original);
    if (original == nullptr) {
        return 7;
    }
    bool header = false;
    Asset* const asset = resourceClass == kGpuTextureClass ? find_asset(tag, header) : nullptr;
    if (asset == nullptr) {
        return original(resourceClass, tag, decoded, decodedSize);
    }
    if (!header) {
        return original(resourceClass, tag, asset->dds.pixels, asset->dds.pixelSize);
    }
    bool report = false;
    const void* const replacement = prepare_header(*asset, decoded, decodedSize, report);
    if (report) {
        report_override(*asset);
    }
    return original(resourceClass,
                    tag,
                    replacement,
                    replacement == decoded ? decodedSize : kTigerTextureHeaderSize);
}

void clear_assets() noexcept {
    AcquireSRWLockExclusive(&g_assetLock);
    g_assets = {};
    ReleaseSRWLockExclusive(&g_assetLock);
}

} // namespace

/** Loads embedded DDS files and attaches the decoded GPU-entry dispatcher. */
bool install(void* module) noexcept {
    if (g_handle.attached) {
        return true;
    }
    if (!core::settings::get().client.customBootflowTextures) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=bootflow_texture stage=setting enabled=0 result=skip");
        return true;
    }
    if (!load_assets(static_cast<HMODULE>(module))) {
        clear_assets();
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootflow_texture stage=resources result=fail");
        return false;
    }
    std::byte* const target =
        scan_main_image_unique(kGpuEntryDispatcher, "bootflow_gpu_entry_dispatcher");
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&dispatch)};
    if (target == nullptr || !hooking::detour::install(spec, g_handle)) {
        clear_assets();
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootflow_texture stage=attach result=fail");
        return false;
    }
    std::array<char, 80> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow_texture stage=attach count=%zu result=ok",
                                      kAssetSpecs.size());
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
    return true;
}

/** Detaches before releasing the resource views and generated Tiger descriptors. */
bool uninstall() noexcept {
    if (g_handle.attached && !hooking::detour::uninstall(g_handle)) {
        return false;
    }
    clear_assets();
    return true;
}

/** @return True while the decoded GPU-entry dispatcher is attached. */
bool is_installed() noexcept {
    return g_handle.attached;
}

} // namespace sunrise::client::hooks::bootflow::texture_override
