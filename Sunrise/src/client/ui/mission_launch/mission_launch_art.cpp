#include "mission_launch_art.h"

#include <algorithm>
#include <cmath>
#include <d3d11.h>

namespace sunrise::client::ui::mission_launch::art {
namespace {
std::array<ID3D11ShaderResourceView*, state::build_data::activities::kIconCount> g_views{};
ID3D11Device* g_device{};
bool g_attempted{};
} // namespace
void release() noexcept {
    for (auto*& view : g_views) {
        if (view != nullptr) {
            view->Release();
            view = nullptr;
        }
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
    g_attempted = false;
}
void prepare(ID3D11Device* device) noexcept {
    if (device == nullptr) {
        return;
    }
    if (device != g_device) {
        release();
        g_device = device;
        g_device->AddRef();
    }
    const auto images = state::build_data::activities::artwork();
    if (g_attempted || images.empty()) {
        return;
    }
    g_attempted = true;
    for (std::size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (image.pixels.empty()) {
            continue;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width;
        desc.Height = image.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = image.pixels.data();
        data.SysMemPitch = static_cast<UINT>(image.width) * 4;
        ID3D11Texture2D* texture{};
        if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)) && texture != nullptr) {
            (void)device->CreateShaderResourceView(texture, nullptr, &g_views[i]);
            texture->Release();
        }
    }
}
std::uint64_t texture(state::build_data::activities::Icon icon) noexcept {
    const auto index = static_cast<std::size_t>(icon);
    return index < g_views.size() ? reinterpret_cast<std::uint64_t>(g_views[index]) : 0;
}
DisplaySize display_size(state::build_data::activities::Icon icon,
                         float extent,
                         float framebufferScale) noexcept {
    namespace catalog = state::build_data::activities;
    if (!std::isfinite(extent) || extent <= 0.0F) {
        return {};
    }
    if (!std::isfinite(framebufferScale) || framebufferScale <= 0.0F) {
        framebufferScale = 1.0F;
    }
    const auto images = catalog::artwork();
    const auto index = static_cast<std::size_t>(icon);
    if (index >= images.size() || images[index].width == 0 || images[index].height == 0) {
        return {};
    }
    const auto& image = images[index];
    float ratio = extent / static_cast<float>((std::max)(image.width, image.height));
    ratio = (std::min)(ratio, 1.0F / framebufferScale);
    return {static_cast<float>(image.width) * ratio, static_cast<float>(image.height) * ratio};
}
} // namespace sunrise::client::ui::mission_launch::art
