#include "ui_picker_component.h"

#include <array>
#include <cstring>
#include <imgui.h>

#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "../filter/ui_filter_component.h"

namespace sunrise::core::ui::components::picker {
namespace {

/** The hidden filter label is scoped by the caller's stable picker ID. */
constexpr char kFilterId[] = "filter";
/** Hint shown while the filter is empty. */
constexpr char kFilterHint[] = "type to filter";
/** Shown in place of the list when the filter matches no row. */
constexpr char kNoMatchLabel[] = "no match";
/** Authored height of the scrolling row area, about 8 rows. */
constexpr float kListHeight = 180.0F;

/**
 * Filter text of the open popup.
 * Dear ImGui keeps one popup open at a time, so one buffer serves every picker. It is cleared on
 * open so a picker never inherits the last one's text.
 */
std::array<char, kFilterCapacity> g_filter{};
/**
 * Which picker's popup is open, or zero.
 * The combo popup uses a derived ID, not the label, so `IsPopupOpen` on the label never matches.
 * Testing it that way re-took keyboard focus every frame and left the rows unclickable.
 */
ImGuiID g_openPicker{};

/** @return The ASCII lowercase form, or the character unchanged. */
[[nodiscard]] char lowercase(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

/**
 * Tests one row against the filter text.
 * @param needle Filter text.
 * @return True when the filter is empty or appears anywhere in the label.
 */
[[nodiscard]] bool matches(const char* label, const char* needle) noexcept {
    if (needle[0] == '\0') {
        return true;
    }
    if (label == nullptr) {
        return false;
    }
    for (std::size_t start = 0; label[start] != '\0'; ++start) {
        std::size_t index = 0;
        while (needle[index] != '\0'
               && lowercase(label[start + index]) == lowercase(needle[index])) {
            ++index;
        }
        if (needle[index] == '\0') {
            return true;
        }
    }
    return false;
}

} // namespace

/** Draws a dropdown whose popup filters its rows as you type. */
bool control(const char* id,
             const char* preview,
             std::span<const Item> items,
             std::size_t& selected) noexcept {
    if (id == nullptr || ImGui::GetCurrentContext() == nullptr) {
        return false;
    }

    ImGui::PushID(id);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const ImGuiID pickerId = ImGui::GetID("##combo");
    bool picked = false;
    // The popup holds a filter row above a fixed-height list, which the default eight-item height
    // would clip.
    if (ImGui::BeginCombo(
            "##combo", preview != nullptr ? preview : "", ImGuiComboFlags_HeightLargest)) {
        // A fresh popup starts with no filter, so the previous picker's text cannot hide every
        // row, and takes focus once so typing filters without a click.
        if (g_openPicker != pickerId) {
            g_openPicker = pickerId;
            g_filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        (void)filter::input(kFilterId, kFilterHint, g_filter.data(), g_filter.size());
        ImGui::Separator();
        if (ImGui::BeginChild("##rows", {0.0F, scaling::dpi::pixels(kListHeight)})) {
            std::size_t shown = 0;
            for (std::size_t index = 0; index < items.size(); ++index) {
                const char* const label = items[index].label;
                if (!matches(label, g_filter.data())) {
                    continue;
                }
                ++shown;
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(label != nullptr ? label : "", index == selected)) {
                    selected = index;
                    picked = true;
                }
                ImGui::PopID();
            }
            if (shown == 0) {
                ImGui::TextUnformatted(kNoMatchLabel);
            }
        }
        ImGui::EndChild();
        if (picked) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
    } else if (g_openPicker == pickerId) {
        g_openPicker = 0;
    }
    ImGui::PopID();
    return picked;
}

} // namespace sunrise::core::ui::components::picker
