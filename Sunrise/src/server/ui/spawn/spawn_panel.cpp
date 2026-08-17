#include "spawn_panel.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <span>
#include <string_view>
#include <vector>

#include "../../../client/content/entity_names/entity_name_cache.h"
#include "../../../client/content/items/packages/internal.h"
#include "../../../client/hooks/spawn/spawn_runtime.h"
#include "../../../core/filesystem/path.h"
#include "../../../core/ui/components/picker/ui_picker_component.h"
#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::server::ui::spawn {
namespace {

namespace native = client::hooks::spawn;
namespace spawn_keys = client::spawn;
namespace package_reader = middleware::content::packages::reader;
namespace picker = core::ui::components::picker;

constexpr std::uint32_t kEntityClass = 0x80809C0FU;
constexpr std::uint8_t kProjectileType = 18;
constexpr std::uint8_t kAmmoType = 20;
constexpr std::uint8_t kLootType = 21;
constexpr std::string_view kNameObject = "\"entities\"";
constexpr std::uint64_t kMaximumNameFile = 32ULL * 1024ULL * 1024ULL;

struct Candidate {
    std::uint32_t tag{};
    std::uint8_t type{};
    bool named{};
    std::array<char, 224> label{};
};

struct EntityName {
    std::uint32_t tag{};
    std::array<char, 144> text{};
    std::uint32_t order{};
};

struct Column {
    std::vector<Candidate> candidates{};
    std::vector<picker::Item> items{};
    std::size_t selected{};
    native::Settings settings{};
    int amount{1};
    int perRow{10};
    float spacing{1.0F};
};

Column g_main{};
Column g_projectile{};
Column g_loot{};
std::vector<EntityName> g_names{};
bool g_scanned{};
std::size_t g_capturingKey{spawn_keys::kActionCount};

void key_name(std::uint32_t virtualKey, std::array<char, 64>& output) noexcept {
    if (virtualKey == spawn_keys::kNoKey) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, 64> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                        wide.data(),
                                                        static_cast<int>(wide.size()))
                                      : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

[[nodiscard]] bool capture_key(std::uint32_t& output) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        output = spawn_keys::kNoKey;
        return true;
    }
    for (int key = 7; key <= 254; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            output = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool key_picker(spawn_keys::Action action,
                              std::uint32_t& virtualKey,
                              float width) noexcept {
    const std::size_t index = static_cast<std::size_t>(action);
    ImGui::PushID(static_cast<int>(index));
    if (g_capturingKey == index) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturingKey = spawn_keys::kActionCount;
        }
        ImGui::PopID();
        std::uint32_t picked = spawn_keys::kNoKey;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturingKey = spawn_keys::kActionCount;
            return true;
        }
        return false;
    }
    std::array<char, 64> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturingKey = index;
    }
    return false;
}

template <std::size_t Capacity>
[[nodiscard]] bool parse_string(std::string_view document,
                                std::size_t& cursor,
                                std::array<char, Capacity>& output) noexcept {
    output = {};
    if (cursor >= document.size() || document[cursor++] != '"') {
        return false;
    }
    std::size_t written = 0;
    while (cursor < document.size()) {
        char value = document[cursor++];
        if (value == '"') {
            return true;
        }
        if (value == '\\') {
            if (cursor >= document.size()) {
                return false;
            }
            value = document[cursor++];
            if (value == 'u') {
                if (cursor + 4 > document.size()) {
                    return false;
                }
                cursor += 4;
                value = '?';
            } else if (value == 'n') {
                value = '\n';
            } else if (value == 'r') {
                value = '\r';
            } else if (value == 't') {
                value = '\t';
            }
        }
        if (written + 1 < output.size()) {
            output[written++] = value;
        }
    }
    return false;
}

void skip_space(std::string_view document, std::size_t& cursor) noexcept {
    while (cursor < document.size()) {
        const char value = document[cursor];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            return;
        }
        ++cursor;
    }
}

[[nodiscard]] bool load_names() noexcept {
    g_names.clear();
    HMODULE module = nullptr;
    constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&load_names), &module) == FALSE) {
        return false;
    }

    std::vector<char> bytes{};
    core::path::Buffer path{};
    if (core::path::artifact_directory(module, path)
        && core::path::append(path, L"\\EntityNames.json")) {
        const HANDLE file = CreateFileW(path.chars.data(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                        nullptr);
        LARGE_INTEGER length{};
        if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &length) != FALSE
            && length.QuadPart > 0
            && static_cast<std::uint64_t>(length.QuadPart) <= kMaximumNameFile) {
            bytes.resize(static_cast<std::size_t>(length.QuadPart));
            DWORD read = 0;
            if (ReadFile(file,
                         bytes.data(),
                         static_cast<DWORD>(bytes.size()),
                         &read,
                         nullptr)
                    == FALSE
                || read != bytes.size()) {
                bytes.clear();
            }
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
    if (bytes.empty()) {
        return false;
    }

    const std::string_view document(bytes.data(), bytes.size());
    std::size_t cursor = document.find(kNameObject);
    cursor = cursor == std::string_view::npos ? cursor : document.find('{', cursor);
    if (cursor == std::string_view::npos) {
        return false;
    }
    ++cursor;
    while (cursor < document.size()) {
        skip_space(document, cursor);
        if (cursor < document.size() && document[cursor] == ',') {
            ++cursor;
            skip_space(document, cursor);
        }
        if (cursor >= document.size() || document[cursor] == '}') {
            break;
        }
        std::array<char, 16> tagText{};
        if (!parse_string(document, cursor, tagText)) {
            return false;
        }
        std::uint32_t tag = 0;
        const char* const end = tagText.data() + std::strlen(tagText.data());
        const auto parsed = std::from_chars(tagText.data(), end, tag, 16);
        skip_space(document, cursor);
        if (parsed.ec != std::errc{} || parsed.ptr != end || cursor >= document.size()
            || document[cursor++] != ':') {
            return false;
        }
        skip_space(document, cursor);
        if (cursor >= document.size() || document[cursor++] != '[') {
            return false;
        }
        std::uint32_t order = 0;
        for (;;) {
            skip_space(document, cursor);
            if (cursor < document.size() && document[cursor] == ',') {
                ++cursor;
                skip_space(document, cursor);
            }
            if (cursor >= document.size()) {
                return false;
            }
            if (document[cursor] == ']') {
                ++cursor;
                break;
            }
            EntityName name{};
            name.tag = tag;
            name.order = order++;
            if (!parse_string(document, cursor, name.text)) {
                return false;
            }
            if (name.text[0] != '\0') {
                g_names.push_back(name);
            }
        }
    }
    std::sort(g_names.begin(), g_names.end(), [](const EntityName& first, const EntityName& second) {
        return first.tag != second.tag ? first.tag < second.tag : first.order < second.order;
    });
    return !g_names.empty();
}

[[nodiscard]] const char* name_of(std::uint32_t tag) noexcept {
    const auto found = std::lower_bound(
        g_names.begin(), g_names.end(), tag, [](const EntityName& value, std::uint32_t wanted) {
            return value.tag < wanted;
        });
    return found != g_names.end() && found->tag == tag ? found->text.data() : nullptr;
}

[[nodiscard]] bool projectile_name(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 12> markers{
        "projectile", "missile", "rocket", "grenade", "fireball", "mortar",
        "cannonball", "seeker", "tracer", "bullet", "plasma_bolt", "weapon_bolt",
    };
    return std::any_of(markers.begin(), markers.end(), [name](std::string_view marker) {
        return name.find(marker) != std::string_view::npos;
    });
}

[[nodiscard]] bool skipped_family(std::wstring_view family) noexcept {
    return family.starts_with(L"w64_audio_") || family.starts_with(L"w64_ui_");
}

void family_text(std::wstring_view family, std::array<char, 96>& output) noexcept {
    output = {};
    const std::size_t count = (std::min)(family.size(), output.size() - 1);
    for (std::size_t index = 0; index < count; ++index) {
        const wchar_t value = family[index];
        output[index] = value >= 32 && value <= 126 ? static_cast<char>(value) : '?';
    }
}

void add_candidate(Column& column,
                   std::uint32_t tag,
                   std::uint8_t type,
                   std::wstring_view family) {
    std::array<char, 96> package{};
    family_text(family, package);
    Candidate value{};
    value.tag = tag;
    value.type = type;
    const char* const name = name_of(tag);
    value.named = name != nullptr;
    if (name != nullptr) {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "%s | 0x%08X | type %u | %s",
                            name,
                            tag,
                            static_cast<unsigned>(type),
                            package.data());
    } else {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "0x%08X | type %u | %s",
                            tag,
                            static_cast<unsigned>(type),
                            package.data());
    }
    column.candidates.push_back(value);
}

bool collect_entity(void*, const package_reader::ClassEntry& entry) noexcept {
    if (skipped_family(entry.packageFamily) || !native::is_tag_resident(entry.tag)) {
        return true;
    }
    std::uint8_t type = 0;
    if (!native::object_type(entry.tag, type)) {
        return true;
    }
    const char* const name = name_of(entry.tag);
    if (type == kProjectileType || (name != nullptr && projectile_name(name))) {
        add_candidate(g_projectile, entry.tag, type, entry.packageFamily);
    } else if (type == kAmmoType || type == kLootType) {
        add_candidate(g_loot, entry.tag, type, entry.packageFamily);
    } else {
        add_candidate(g_main, entry.tag, type, entry.packageFamily);
    }
    return true;
}

void finish_column(Column& column) {
    std::sort(column.candidates.begin(),
              column.candidates.end(),
              [](const Candidate& first, const Candidate& second) {
                  if (first.named != second.named) {
                      return first.named;
                  }
                  return std::string_view(first.label.data())
                         < std::string_view(second.label.data());
              });
    column.candidates.erase(
        std::unique(column.candidates.begin(),
                    column.candidates.end(),
                    [](const Candidate& first, const Candidate& second) {
                        return first.tag == second.tag;
                    }),
        column.candidates.end());
    column.items.clear();
    column.items.reserve(column.candidates.size());
    for (const Candidate& candidate : column.candidates) {
        column.items.push_back({candidate.label.data()});
    }
    column.selected = 0;
}

void refresh() noexcept {
    g_main.candidates.clear();
    g_projectile.candidates.clear();
    g_loot.candidates.clear();
    core::path::Buffer directory{};
    const bool hasDirectory = client::content::items::packages::package_directory(directory);
    if (hasDirectory) {
        (void)client::content::entity_names::ensure(directory.chars.data());
    }
    (void)load_names();
    if (native::ready() && hasDirectory) {
        package_reader::ScanResult result{};
        (void)package_reader::scan_class_entries(
            directory.chars.data(), kEntityClass, &collect_entity, nullptr, result);
        package_reader::release_caches();
    }
    finish_column(g_main);
    finish_column(g_projectile);
    finish_column(g_loot);
    g_scanned = true;
}

[[nodiscard]] const char* preview(const Column& column) noexcept {
    return column.selected < column.candidates.size()
               ? column.candidates[column.selected].label.data()
               : "[None]";
}

void draw_settings(Column& column, const char* id, bool showSpawnAll) noexcept {
    ImGui::PushID(id);
    ImGui::TextUnformatted("Amount:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragInt("##amount", &column.amount, 1.0F, 1, 4096, "%d");
    ImGui::TextUnformatted("Vertical lift:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat(
        "##vertical_lift", &column.settings.lift, 0.1F, -100.0F, 100.0F, "%.1f");
    ImGui::TextUnformatted("Ray distance:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##ray_distance",
                     &column.settings.rayDistance,
                     1.0F,
                     1.0F,
                     2000.0F,
                     "%.0f");
    ImGui::TextUnformatted("Scale:");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##scale", &column.settings.scale, 0.01F, 0.01F, 100.0F, "%.2f");
    ImGui::Checkbox("Camera rotation", &column.settings.useCameraRotation);

    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Override rotation", &column.settings.overrideRotation);
        ImGui::TextUnformatted("Position offset:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat3("##position_offset", column.settings.offset.data(), "%.2f");
        if (column.settings.overrideRotation) {
            ImGui::TextUnformatted("Rotation quaternion:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat4("##rotation_quaternion", column.settings.rotation.data(), "%.3f");
        }
        ImGui::TreePop();
    }

    if (showSpawnAll
        && ImGui::TreeNodeEx("Spawn All [unstable]", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::TextUnformatted("Items per row:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt("##items_per_row", &column.perRow, 1.0F, 1, 4096, "%d");
        ImGui::TextUnformatted("Spacing:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##spacing", &column.spacing, 0.1F, 0.1F, 100.0F, "%.1f");
        if (ImGui::Button("Spawn all at crosshair", ImVec2(-FLT_MIN, 0.0F))) {
            std::vector<std::uint32_t> tags{};
            tags.reserve(column.candidates.size());
            for (const Candidate& candidate : column.candidates) {
                tags.push_back(candidate.tag);
            }
            (void)native::request_line(tags,
                                       native::Origin::crosshair,
                                       static_cast<std::uint32_t>((std::max)(column.perRow, 1)),
                                       column.spacing,
                                       column.settings);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_keybinds(spawn_keys::Action playerAction,
                   spawn_keys::Action crosshairAction,
                   spawn_keys::Keybinds& keybinds,
                   bool& changed) noexcept {
    if (!ImGui::TreeNodeEx("Keybinds", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    const float labelWidth = ImGui::CalcTextSize("At crosshair").x
                             + ImGui::GetStyle().ItemSpacing.x * 2.0F;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At player");
    ImGui::SameLine(labelWidth);
    changed = key_picker(playerAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(playerAction)],
                         controlWidth)
              || changed;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At crosshair");
    ImGui::SameLine(labelWidth);
    changed = key_picker(crosshairAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(crosshairAction)],
                         controlWidth)
              || changed;
    ImGui::TreePop();
}

void draw_column(const char* title,
                 const char* id,
                 Column& column,
                 spawn_keys::Action playerAction,
                 spawn_keys::Action crosshairAction,
                 bool showSpawnAll,
                 spawn_keys::Keybinds& keybinds,
                 bool& keybindsChanged) noexcept {
    ImGui::PushID(id);
    ImGui::TextUnformatted(title);
    ImGui::Separator();
    const std::span<const picker::Item> rows(column.items.data(), column.items.size());
    (void)picker::control("picker", preview(column), rows, column.selected);

    ImGui::BeginDisabled(column.selected >= column.candidates.size() || native::busy());
    if (ImGui::Button("At player", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
        (void)native::request(column.candidates[column.selected].tag,
                              native::Origin::player,
                              static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                              column.settings);
    }
    ImGui::SameLine();
    if (ImGui::Button("At crosshair", ImVec2(-FLT_MIN, 0.0F))) {
        (void)native::request(column.candidates[column.selected].tag,
                              native::Origin::crosshair,
                              static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                              column.settings);
    }
    ImGui::EndDisabled();
    const std::uint32_t selectedTag = column.selected < column.candidates.size()
                                          ? column.candidates[column.selected].tag
                                          : 0xFFFFFFFFU;
    const std::uint32_t amount = static_cast<std::uint32_t>((std::max)(column.amount, 1));
    native::configure_shortcut(playerAction, selectedTag, amount, column.settings);
    native::configure_shortcut(crosshairAction, selectedTag, amount, column.settings);
    draw_keybinds(playerAction, crosshairAction, keybinds, keybindsChanged);
    draw_settings(column, "settings", showSpawnAll);
    ImGui::PopID();
}

} // namespace

void draw() noexcept {
    if (!g_scanned) {
        refresh();
    }

    if (ImGui::Button("Refresh loaded entities")) {
        refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu main  |  %zu projectiles  |  %zu loot",
                        g_main.candidates.size(),
                        g_projectile.candidates.size(),
                        g_loot.candidates.size());
    if (native::busy()) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            native::cancel();
        }
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame
                                      | ImGuiTableFlags_BordersInnerV
                                      | ImGuiTableFlags_PadOuterX;
    spawn_keys::Keybinds keybinds = spawn_keys::get();
    bool keybindsChanged = false;
    if (ImGui::BeginTable("spawn_columns", 3, flags)) {
        ImGui::TableNextColumn();
        draw_column("Main spawner",
                    "main",
                    g_main,
                    spawn_keys::Action::mainPlayer,
                    spawn_keys::Action::mainCrosshair,
                    false,
                    keybinds,
                    keybindsChanged);
        ImGui::TableNextColumn();
        draw_column("Projectile spawner",
                    "projectile",
                    g_projectile,
                    spawn_keys::Action::projectilePlayer,
                    spawn_keys::Action::projectileCrosshair,
                    true,
                    keybinds,
                    keybindsChanged);
        ImGui::TableNextColumn();
        draw_column("Loot spawner",
                    "loot",
                    g_loot,
                    spawn_keys::Action::lootPlayer,
                    spawn_keys::Action::lootCrosshair,
                    true,
                    keybinds,
                    keybindsChanged);
        ImGui::EndTable();
    }
    if (keybindsChanged) {
        (void)spawn_keys::publish(keybinds);
    }
}

} // namespace sunrise::server::ui::spawn
