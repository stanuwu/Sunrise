#include <Windows.h>

#include <backends/imgui_impl_dx11.h>
#include <cassert>
#include <crtdbg.h>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <map>
#include <string>
#include <vector>
#include <wincodec.h>

#include "../Sunrise/src/core/ui/layout/ui_layout_render.cpp"
#include "../Sunrise/src/core/ui/textures/ui_texture_slots.h"
#include "../Sunrise/src/core/ui/theme/sunrise_ui_theme.h"

using namespace sunrise::core::ui;

static layout::StateSnapshot selectionState{true};
static layout::workspace::State saved{};
static unsigned saves{}, worldDraws{}, deactivations{};
static bool worldHelpers{};
static float displayScale = 1;
static float packetLabelWidth{};
static std::map<ImGuiID, ImRect> rectangles;
static std::map<std::string, ImRect> labels;
static ID3D11Device* device{};
static ID3D11DeviceContext* context{};
static ID3D11Texture2D* logoTexture{};
static ID3D11ShaderResourceView* logoView{};

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*,
                                 ImGuiID id,
                                 const ImRect& bounds,
                                 const ImGuiLastItemData*) {
    rectangles[id] = bounds;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* contextValue,
                                  ImGuiID id,
                                  const char* label,
                                  ImGuiItemStatusFlags) {
    if (rectangles.contains(id)) {
        labels[label] = rectangles[id];
        if (contextValue->CurrentWindow
            && (contextValue->CurrentWindow->Flags & ImGuiWindowFlags_Popup))
            labels[std::string("popup:") + label] = rectangles[id];
    }
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) {
    return "";
}

namespace sunrise::core::ui::layout {
StateSnapshot snapshot() noexcept {
    return selectionState;
}
namespace internal {
bool context_is_current() noexcept {
    return true;
}
void select_module(std::string_view stableId) noexcept {
    selectionState.selectedStableId = {};
    std::copy(stableId.begin(), stableId.end(), selectionState.selectedStableId.begin());
    selectionState.selectedStableIdLength = stableId.size();
}
} // namespace internal
namespace workspace {
void load(State& state) noexcept {
    state = saved;
}
bool save(const State& state) noexcept {
    saved = state;
    ++saves;
    return true;
}
} // namespace workspace
} // namespace sunrise::core::ui::layout

namespace sunrise::core::ui::scaling::dpi {
float current() noexcept {
    return displayScale;
}
float pixels(float value) noexcept {
    return value * displayScale;
}
ImVec2 pixels(const ImVec2& value) noexcept {
    return {value.x * displayScale, value.y * displayScale};
}
} // namespace sunrise::core::ui::scaling::dpi

static void frame(bool visible = true) {
    rectangles.clear();
    labels.clear();
    theme::apply();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    (void)layout::render(visible);
    ImGui::Render();
}
static void move_mouse(ImVec2 point) {
    ImGui::GetIO().AddMousePosEvent(point.x, point.y);
    frame();
}
static void click_at(ImVec2 point) {
    move_mouse(point);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame();
    frame();
}
static void double_click_at(ImVec2 point) {
    move_mouse(point);
    for (int clickIndex = 0; clickIndex < 2; ++clickIndex) {
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frame();
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        frame();
    }
    frame();
}
static void click(const char* label) {
    const auto popup = std::string("popup:") + label;
    const auto key = labels.contains(popup) ? popup : std::string(label);
    assert(labels.contains(key));
    click_at(labels.at(key).GetCenter());
}
static ImRect tab_rect(const char* stableId, bool close = false) {
    for (auto* window : GImGui->Windows) {
        if (std::string(window->Name).find("/##tabs_") == std::string::npos) continue;
        const auto seed = ImHashStr(stableId, 0, window->ID);
        const auto id = ImHashStr(close ? "##close_tab" : "##tab", 0, seed);
        if (rectangles.contains(id)) return rectangles.at(id);
    }
    assert(false);
    return {};
}
static std::string selected_tab() {
    return layout::g_workspace.selected.data();
}
static std::string selected_module() {
    return {selectionState.selectedStableId.data(), selectionState.selectedStableIdLength};
}
static ImGuiWindow* active_workspace_content() {
    for (auto* window : GImGui->Windows)
        if (window->Active
            && std::string_view(window->Name).find("##workspace_content") != std::string_view::npos)
            return window;
    return nullptr;
}
static void on_deactivate() noexcept {
    worldHelpers = false;
    ++deactivations;
}
static void world_page() noexcept {
    ++worldDraws;
    worldHelpers = true;
    ImGui::SetNextItemWidth(-65);
    if (ImGui::BeginCombo("Instance", "edz_freeroam (active, linked)")) ImGui::EndCombo();
    ImGui::Spacing();
    ImGui::TextDisabled("SDK ready   |   Current activity");
    ImGui::Separator();
    ImGui::Spacing();
    const float navigationWidth = 110 * displayScale;
    ImGui::BeginChild("##fixture_navigation", {navigationWidth, 0});
    for (const auto* name : {"Overview", "Objects", "Squads", "Triggers", "Variables"})
        ImGui::Selectable(name, std::string_view(name) == "Objects");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##fixture_objects", {0, 0});
    ImGui::TextDisabled("Objects in the selected activity");
    if (ImGui::BeginTable("##fixture_table", 3, ImGuiTableFlags_RowBg)) {
        for (const auto* name : {"trostland", "sludge", "outskirts", "winding_cove"}) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Area");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Available");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}
static void packets_page() noexcept {
    packetLabelWidth = ImGui::CalcTextSize("Packets").x;
    static bool followPlayer = true;
    ImGui::Checkbox("Follow player", &followPlayer);
    ImGui::SetNextItemWidth(-65);
    if (ImGui::BeginCombo("Instance", "edz_freeroam (active, linked)")) ImGui::EndCombo();
    ImGui::Separator();
    components::section::header("Incoming packets");
    if (ImGui::BeginTable(
            "##packet_fixture", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Packet", ImGuiTableColumnFlags_WidthStretch, 2);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto* name : {"patch_epoch", "send_client_heartbeat", "sensor_sense_update"}) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("46");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("decoded");
        }
        ImGui::EndTable();
    }
    ImGui::Dummy({0, 800 * displayScale});
}
static void main_page() noexcept {
    ImGui::TextUnformatted("Tool settings");
    ImGui::Separator();
    ImGui::TextDisabled("This page remains in the main Sunrise layout.");
    ImGui::Dummy({0, 300});
}
static void activity_host_page() noexcept {
    static bool followPlayer = true;
    ImGui::Checkbox("Follow player", &followPlayer);
    ImGui::SetNextItemWidth(-70 * displayScale);
    if (ImGui::BeginCombo("Instance", "edz_freeroam (active, linked)")) ImGui::EndCombo();
    ImGui::Spacing();
    ImGui::TextDisabled("WINDOWS");
    ImGui::Separator();
    bool world = layout::workspace_tab_open("server.world");
    if (ImGui::Checkbox("World", &world)) layout::set_workspace_tab_open("server.world", world);
    ImGui::SameLine();
    bool packets = layout::workspace_tab_open("server.packets");
    if (ImGui::Checkbox("Packets", &packets))
        layout::set_workspace_tab_open("server.packets", packets);
    if (ImGui::Button("Open all")) {
        layout::set_workspace_tab_open("server.world", true);
        layout::set_workspace_tab_open("server.packets", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close all")) {
        layout::set_workspace_tab_open("server.world", false);
        layout::set_workspace_tab_open("server.packets", false);
    }
}
static void register_page(const char* id,
                          const char* name,
                          modules::FrameCallback callback,
                          bool workspaceTab = false,
                          modules::FrameCallback deactivate = nullptr) {
    modules::Descriptor descriptor;
    assert(modules::create_descriptor(modules::Owner::core, id, name, callback, descriptor));
    descriptor = descriptor.with_deactivation(deactivate);
    if (workspaceTab)
        descriptor = descriptor.with_presentation(modules::Presentation::workspaceTab);
    assert(modules::registry::register_module(descriptor)
           == modules::registry::RegistrationResult::registered);
}

static void capture(const std::filesystem::path& path) {
    const auto size = ImGui::GetIO().DisplaySize;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(size.x);
    description.Height = static_cast<UINT>(size.y);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target{};
    assert(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &target)));
    ID3D11RenderTargetView* renderTarget{};
    assert(SUCCEEDED(device->CreateRenderTargetView(target, nullptr, &renderTarget)));
    context->OMSetRenderTargets(1, &renderTarget, nullptr);
    const float background[]{.08F, .095F, .12F, 1};
    context->ClearRenderTargetView(renderTarget, background);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging{};
    assert(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &staging)));
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << description.Width << " " << description.Height << "\n255\n";
    for (UINT y = 0; y < description.Height; ++y)
        for (UINT x = 0; x < description.Width; ++x) {
            const auto* pixel =
                static_cast<const char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            file.write(pixel, 3);
        }
    context->Unmap(staging, 0);
    staging->Release();
    renderTarget->Release();
    target->Release();
}

static bool load_logo(const std::filesystem::path& path) {
    IWICImagingFactory* factory{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frameValue{};
    IWICFormatConverter* converter{};
    UINT width{}, height{};
    bool okay = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
                && SUCCEEDED(CoCreateInstance(
                    CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
                && SUCCEEDED(factory->CreateDecoderFromFilename(
                    path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frameValue))
                && SUCCEEDED(factory->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frameValue,
                                                   GUID_WICPixelFormat32bppRGBA,
                                                   WICBitmapDitherTypeNone,
                                                   nullptr,
                                                   0,
                                                   WICBitmapPaletteTypeCustom))
                && SUCCEEDED(converter->GetSize(&width, &height));
    std::vector<BYTE> pixels;
    if (okay) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        okay = SUCCEEDED(converter->CopyPixels(
            nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    }
    if (okay) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
        okay = SUCCEEDED(device->CreateTexture2D(&description, &initial, &logoTexture))
               && SUCCEEDED(device->CreateShaderResourceView(logoTexture, nullptr, &logoView));
    }
    if (converter) converter->Release();
    if (frameValue) frameValue->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (okay) textures::publish(textures::Slot::logoSheet, reinterpret_cast<ImTextureID>(logoView));
    return okay;
}

int main(int argc, char** argv) {
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = {2048, 857};
    ImGui::GetIO().DeltaTime = .1F;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,
                                       D3D_DRIVER_TYPE_WARP,
                                       nullptr,
                                       0,
                                       nullptr,
                                       0,
                                       D3D11_SDK_VERSION,
                                       &device,
                                       nullptr,
                                       &context)));
    ImFontConfig fontConfig{};
    fontConfig.SizePixels = 16;
    fontConfig.RasterizerDensity = 2;
    if (argc > 2)
        ImGui::GetIO().FontDefault =
            ImGui::GetIO().Fonts->AddFontFromFileTTF(argv[2], 16, &fontConfig);
    else
        ImGui::GetIO().FontDefault = ImGui::GetIO().Fonts->AddFontDefaultVector(&fontConfig);
    assert(ImGui::GetIO().FontDefault);
    ImGui::GetStyle().FontSizeBase = 16;
    ImGui_ImplDX11_Init(device, context);
    theme::apply();
    if (argc > 3) assert(load_logo(argv[3]));
    GImGui->TestEngineHookItems = true;

    register_page("client.movement", "Movement", main_page);
    register_page("client.player", "Player", main_page);
    register_page("server.activity", "Activity", main_page);
    register_page("server.activity_host", "Activity Host", activity_host_page);
    register_page("server.world", "World", world_page, true, on_deactivate);
    register_page("server.packets", "Packets", packets_page, true);
    register_page("core.hud", "HUD", main_page);
    register_page("core.logs", "Logs", main_page);

    for (int i = 0; i < 20; ++i)
        frame();
    assert(selected_tab() == layout::workspace::kMainTabId);
    assert(selected_module() == "client.movement");
    assert(layout::g_workspace.tabCount == 1 && !worldHelpers);
    layout::open_workspace_tab("client.player");
    assert(layout::g_workspace.tabCount == 1);

    click("Activity Host");
    assert(selected_module() == "server.activity_host");
    click("World");
    assert(selected_tab() == "server.world" && worldHelpers);
    assert(layout::g_workspace.tabCount == 2);

    ImGui::GetIO().DisplaySize = {0, 0};
    frame(false);
    assert(!worldHelpers);
    ImGui::GetIO().DisplaySize = {2048, 857};
    frame();
    frame();
    assert(worldHelpers);
    ImGui::GetIO().DisplaySize = {0, 0};
    frame(true);
    assert(!worldHelpers);
    ImGui::GetIO().DisplaySize = {2048, 857};
    frame();
    frame();
    assert(worldHelpers);

    click("##sunrise_home");
    assert(selected_tab() == layout::workspace::kMainTabId && !worldHelpers);
    assert(selected_module() == "server.activity_host");
    click("Packets");
    assert(selected_tab() == "server.packets" && layout::g_workspace.tabCount == 3);
    // The full text plus a visible gap must fit before the close button at every supported DPI.
    for (const float scale : {0.9F, 1.0F, 1.6875F, 2.5F}) {
        displayScale = scale;
        frame();
        frame();
        const auto tab = tab_rect("server.packets");
        const auto close = tab_rect("server.packets", true);
        assert(packetLabelWidth + 16 * scale <= close.Min.x - tab.Min.x + 0.5F);
        assert(labels.at("##workspace_menu").Max.x <= ImGui::GetIO().DisplaySize.x);
    }
    displayScale = 1;
    frame();
    frame();
    assert(active_workspace_content() != nullptr);
    active_workspace_content()->Scroll.y = 100;
    click("##sunrise_home");
    click_at(tab_rect("server.world").GetCenter());
    assert(selected_tab() == "server.world" && layout::g_workspace.tabCount == 3);

    const auto from = tab_rect("server.world").GetCenter();
    const auto to = tab_rect("server.packets").GetCenter();
    move_mouse(from);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame();
    move_mouse({from.x + 10, from.y});
    move_mouse(to);
    frame();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame();
    frame();
    assert(std::string(layout::g_workspace.tabs[2].data()) == "server.world");
    click_at(tab_rect("server.world", true).GetCenter());
    assert(selected_tab() == "server.packets" && !worldHelpers);
    click("##sunrise_home");
    click("World");
    click_at(tab_rect("server.packets").GetCenter());
    assert(active_workspace_content() != nullptr && active_workspace_content()->Scroll.y == 100);
    click_at(tab_rect("server.packets", true).GetCenter());
    assert(selected_tab() == "server.world" && worldHelpers);

    assert(!labels.contains("##minimize") && !labels.contains("##maximize"));
    const auto full = layout::g_workspace.restore;
    double_click_at(labels.at("##drag_strip").GetCenter());
    assert(layout::g_workspace.maximized && layout::g_workspace.restore == full);
    double_click_at(labels.at("##drag_strip").GetCenter());
    assert(!layout::g_workspace.maximized && layout::g_workspace.restore == full);

    const auto priorPosition = layout::g_workspace.restore;
    move_mouse(labels.at("##sunrise_home").GetCenter());
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame();
    const auto mouse = ImGui::GetIO().MousePos;
    const auto saveCount = saves;
    move_mouse({mouse.x + 40, mouse.y + 20});
    assert(saves == saveCount);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame();
    frame();
    assert(layout::g_workspace.restore.x > priorPosition.x);

    const auto drawsBeforeHide = worldDraws;
    const auto deactivationsBeforeHide = deactivations;
    for (int i = 0; i < 20; ++i) {
        frame(false);
        assert(worldHelpers && worldDraws == drawsBeforeHide
               && deactivations == deactivationsBeforeHide);
    }
    assert(!labels.contains("##workspace_menu"));
    for (int i = 0; i < 20; ++i)
        frame();
    assert(worldHelpers);
    const auto remembered = layout::g_workspace;
    layout::internal::reset_workspace();
    frame();
    frame();
    assert(layout::g_workspace == remembered);

    for (int i = 0; i < 7; ++i) {
        const std::string id = "tool.aux" + std::to_string(i);
        const std::string name = "Auxiliary " + std::to_string(i);
        register_page(id.c_str(), name.c_str(), main_page, true);
        layout::open_workspace_tab(id);
    }
    ImGui::GetIO().DisplaySize = {620, 420};
    frame();
    frame();
    assert(labels.contains("##scroll_right"));
    const auto activeTab = tab_rect(layout::g_workspace.selected.data());
    assert(activeTab.Max.x <= labels.at("##scroll_left").Min.x);
    click("##workspace_menu");
    click("Sunrise");
    assert(selected_tab() == layout::workspace::kMainTabId);
    displayScale = 1.5F;
    ImGui::GetIO().DisplaySize = {930, 630};
    frame();
    frame();
    assert(ImGui::FindWindowByName("Sunrise")->Size.x == 620 * 1.5F);
    displayScale = 1;
    ImGui::GetIO().DisplaySize = {420, 300};
    frame();
    frame();
    assert(labels.at("##workspace_menu").Max.x <= 420);
    assert(labels.contains("World") && labels.contains("Packets") && labels.contains("Open all")
           && labels.contains("Close all"));
    click("Close all");
    assert(!layout::workspace_tab_open("server.world")
           && !layout::workspace_tab_open("server.packets"));
    click("World");
    assert(selected_tab() == "server.world");
    click("##sunrise_home");
    click("Packets");
    assert(selected_tab() == "server.packets");
    click("##sunrise_home");
    const auto tabsBeforeReset = layout::g_workspace.tabCount;
    click("##workspace_menu");
    click("Reset layout");
    assert(layout::g_workspace.tabCount == tabsBeforeReset);

    layout::open_workspace_tab("server.world");
    frame();
    assert(worldHelpers);
    for (int i = 0; i < 20; ++i)
        frame(false);
    layout::set_workspace_tab_open("server.world", false);
    frame(false);
    assert(!worldHelpers);
    layout::open_workspace_tab("server.world");
    frame();
    assert(worldHelpers);
    for (int i = 0; i < 20; ++i)
        frame(false);
    assert(modules::registry::unregister_module("server.world"));
    frame(false);
    assert(!worldHelpers && selected_tab() != "server.world");
    while (layout::g_workspace.tabCount > 1)
        layout::workspace::close(layout::g_workspace, 1);
    frame();
    assert(layout::g_workspace.tabCount == 1 && selected_tab() == layout::workspace::kMainTabId);

    if (argc > 1) {
        const std::filesystem::path directory = argv[1];
        std::filesystem::create_directories(directory);
        // Capture fresh pages rather than scroll positions left by interaction regressions.
        for (auto* window : GImGui->Windows)
            window->Scroll = {};
        ImGui::GetIO().AddMousePosEvent(-1000, -1000);
        register_page("server.world", "World", world_page, true, on_deactivate);
        ImGui::GetIO().DisplaySize = {2048, 857};
        layout::g_workspace = {};
        layout::g_workspace.restore = {450, 120, 1140, 620};
        layout::g_workspace.positioned = true;
        layout::open_workspace_tab("server.world");
        layout::open_workspace_tab("server.packets");
        layout::activate(layout::workspace::kMainTabId);
        layout::internal::select_module("server.activity_host");
        for (int i = 0; i < 20; ++i)
            frame();
        capture(directory / "workspace-main-wide.ppm");
        layout::open_workspace_tab("server.world");
        frame();
        capture(directory / "workspace-world-wide.ppm");
        layout::open_workspace_tab("server.packets");
        for (int i = 0; i < 3; ++i)
            frame();
        capture(directory / "workspace-packets-wide.ppm");
        displayScale = 1.6875F;
        ImGui::GetIO().DisplaySize = {1920, 1080};
        layout::g_workspace.restore = {60, 50, 940, 560};
        for (int i = 0; i < 3; ++i)
            frame();
        capture(directory / "workspace-packets-dpi.ppm");
        displayScale = 1;
        ImGui::GetIO().DisplaySize = {620, 480};
        layout::g_workspace.restore = {0, 0, 620, 480};
        layout::activate(layout::workspace::kMainTabId);
        layout::g_revealSelected = true;
        for (int i = 0; i < 3; ++i)
            frame();
        capture(directory / "workspace-main-narrow.ppm");
        ImGui::GetIO().DisplaySize = {420, 420};
        layout::g_workspace.restore = {0, 0, 420, 420};
        layout::g_revealSelected = true;
        for (int i = 0; i < 3; ++i)
            frame();
        capture(directory / "workspace-main-minimum.ppm");
        register_page("test.long_label", "Geometry and activity diagnostics", main_page, true);
        layout::open_workspace_tab("test.long_label");
        for (int i = 0; i < 3; ++i)
            frame();
        capture(directory / "workspace-overflow.ppm");
        click("##workspace_menu");
        capture(directory / "workspace-overflow-menu.ppm");
    }

    layout::internal::reset_workspace();
    textures::clear();
    if (logoView) logoView->Release();
    if (logoTexture) logoTexture->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    context->Release();
    device->Release();
    CoUninitialize();
}
