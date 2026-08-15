#include "logs_filter_controls.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <imgui.h>
#include <string_view>

#include "../../../components/filter/ui_filter_component.h"
#include "../../../scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::core::ui::modules::logs::internal::filters {
namespace {

/** One null byte lets Dear ImGui edit the full text-filter buffer. */
constexpr std::size_t kInputTerminatorBytes = 1;
/** 140 pixels fit every channel name without crowding the row. */
constexpr float kChannelFilterWidth = 140.0F;
/** 110 pixels fit every severity name without crowding the row. */
constexpr float kLevelFilterWidth = 110.0F;

struct ChannelChoice {
    const char* label;
    bool filtered;
    log::Channel channel;
};

struct LevelChoice {
    const char* label;
    bool filtered;
    log::Level level;
};

/** Ordered choices map the channel combo index to one exact logger channel. */
constexpr std::array<ChannelChoice, 6> kChannelChoices{{
    {"All channels", false, log::Channel::core},
    {"Core", true, log::Channel::core},
    {"Client", true, log::Channel::client},
    {"State", true, log::Channel::state},
    {"Server", true, log::Channel::server},
    {"Middleware", true, log::Channel::middleware},
}};
/** Ordered choices map the level combo index to one exact emitted severity. */
constexpr std::array<LevelChoice, 5> kLevelChoices{{
    {"All levels", false, log::Level::error},
    {"Error", true, log::Level::error},
    {"Warn", true, log::Level::warn},
    {"Info", true, log::Level::info},
    {"Debug", true, log::Level::debug},
}};

struct SelectionState {
    std::size_t channelChoice{};
    std::size_t levelChoice{};
    std::array<char, log::view::kTextFilterCapacity + kInputTerminatorBytes> text{};
};

SelectionState g_selection;

/** @return Current query text from the fixed edit buffer. */
[[nodiscard]] std::string_view text_query() noexcept {
    const auto end = std::find(g_selection.text.begin(), g_selection.text.end(), '\0');
    return {g_selection.text.data(), static_cast<std::size_t>(end - g_selection.text.begin())};
}

} // namespace

/** Draws the exact-channel selector and stores the selection. */
void draw_channel() noexcept {
    ImGui::SetNextItemWidth(scaling::dpi::pixels(kChannelFilterWidth));
    if (!ImGui::BeginCombo("##log_channel", kChannelChoices[g_selection.channelChoice].label)) {
        return;
    }
    for (std::size_t index = 0; index < kChannelChoices.size(); ++index) {
        const bool selected = index == g_selection.channelChoice;
        if (ImGui::Selectable(kChannelChoices[index].label, selected)) {
            g_selection.channelChoice = index;
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

/** Draws the exact-level selector and stores the selection. */
void draw_level() noexcept {
    ImGui::SetNextItemWidth(scaling::dpi::pixels(kLevelFilterWidth));
    if (!ImGui::BeginCombo("##log_level", kLevelChoices[g_selection.levelChoice].label)) {
        return;
    }
    for (std::size_t index = 0; index < kLevelChoices.size(); ++index) {
        const bool selected = index == g_selection.levelChoice;
        if (ImGui::Selectable(kLevelChoices[index].label, selected)) {
            g_selection.levelChoice = index;
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

/** Draws the text query input and stores the query. */
void draw_text() noexcept {
    (void)components::filter::input(
        "log_text", "Filter text", g_selection.text.data(), g_selection.text.size());
}

/** @return A filter built from the current selections. */
log::view::Filter current() noexcept {
    log::view::Filter filter;
    const ChannelChoice& channelChoice = kChannelChoices[g_selection.channelChoice];
    if (channelChoice.filtered) {
        filter.channel = channelChoice.channel;
    }
    const LevelChoice& levelChoice = kLevelChoices[g_selection.levelChoice];
    if (levelChoice.filtered) {
        filter.level = levelChoice.level;
    }
    (void)log::view::set_text(filter, text_query());
    return filter;
}

/** Clears every selection at a UI lifecycle boundary. */
void reset() noexcept {
    g_selection = {};
}

} // namespace sunrise::core::ui::modules::logs::internal::filters
