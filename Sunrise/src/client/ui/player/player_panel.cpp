/** The player module's interface. Every control saves at once, so a change survives a restart. */

#include "player_panel.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::player {
namespace {

/** Season values change rarely, so they are reread at this interval instead of every frame. */
constexpr std::uint64_t kSeasonRefreshIntervalMs = 1'000;

/** Season values last read from State. */
struct SeasonValues {
    std::int32_t experience{};
    std::uint16_t rank{};
    std::uint16_t powerBonus{};
    bool complete{};
    std::uint64_t readTick{};
    bool valid{};
};

SeasonValues g_season{};
/** Last grant or reset outcome, or null before the first one. */
const char* g_seasonFeedback = nullptr;

/** Rereads the Season values once the interval has passed or an action invalidated them. */
void refresh_season() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (g_season.valid && now - g_season.readTick < kSeasonRefreshIntervalMs) {
        return;
    }
    g_season.experience = state::seasonal_experience();
    g_season.rank = state::seasonal_rank();
    g_season.powerBonus = state::artifact_power_bonus();
    g_season.complete = state::seasonal_experience_to_advance(1) == 0;
    g_season.readTick = now;
    g_season.valid = true;
}

/** Writes XP with a comma between thousands, the way the game prints XP. */
void format_experience(std::int32_t experience, std::array<char, 16>& output) noexcept {
    std::array<char, 12> digits{};
    const int count = std::snprintf(digits.data(), digits.size(), "%d", experience);
    std::size_t written = 0;
    for (int index = 0; index < count; ++index) {
        if (index > 0 && digits[index - 1] != '-' && (count - index) % 3 == 0) {
            output[written++] = ',';
        }
        output[written++] = digits[index];
    }
    output[written] = '\0';
}

/** Keeps an action's outcome and rereads the values on the next frame. */
void finish_season_action(bool done, const char* success, const char* failure) noexcept {
    g_seasonFeedback = done ? success : failure;
    g_season.valid = false;
}

/** Grants whole ranks on a press. */
void season_rank_button(const char* label, std::uint16_t ranks, const char* success) noexcept {
    if (ImGui::Button(label)) {
        finish_season_action(server::bap::grant_seasonal_ranks(ranks), success, "Grant failed.");
    }
}

/** Shows the account's Season progress, the grants and the reset. */
void draw_season_progression() noexcept {
    refresh_season();
    ImGui::TextUnformatted("Season Progression");
    ImGui::Separator();
    ImGui::TextWrapped("Increase the Season rank without losing current XP progress");
    ImGui::Spacing();
    ImGui::Text("Season Rank: %u", static_cast<unsigned>(g_season.rank));
    std::array<char, 16> experience{};
    format_experience(g_season.experience, experience);
    ImGui::Text("Seasonal XP: %s", experience.data());
    ImGui::Text("Artifact Power Bonus: +%u", static_cast<unsigned>(g_season.powerBonus));
    ImGui::Spacing();
    ImGui::BeginDisabled(g_season.complete);
    if (ImGui::Button("+50K XP##season_xp_50k")) {
        finish_season_action(
            server::bap::grant_seasonal_experience_capped(50'000), "XP granted.", "Grant failed.");
    }
    ImGui::SameLine();
    season_rank_button("+1 Rank##season_rank_1", 1, "Ranks granted.");
    ImGui::SameLine();
    season_rank_button("+5 Ranks##season_rank_5", 5, "Ranks granted.");
    ImGui::SameLine();
    // A successful Max Rank grant always stops exactly at the rank-100 threshold.
    season_rank_button("Max Rank##season_rank_max", 100, "Rank 100 reached.");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(g_season.experience == 0);
    if (ImGui::Button("Reset Season Progression##season_reset")) {
        finish_season_action(server::bap::reset_seasonal_progression(),
                             "Season progression reset.",
                             "Reset failed.");
    }
    ImGui::EndDisabled();
    if (g_seasonFeedback != nullptr) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_seasonFeedback);
    }
}

} // namespace

/** Draws the player module inside the active Core UI frame. */
void draw() noexcept {
    namespace toggle = core::ui::components::toggle;
    client::player::Settings settings = client::player::get();

    ImGui::TextUnformatted("Infinite Ammo");
    ImGui::Separator();
    ImGui::TextWrapped("Keep every weapon's reserves full.");
    ImGui::Spacing();
    bool changed = toggle::control("Enabled##infinite_ammo", settings.infiniteAmmoEnabled);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Anti AFK");
    ImGui::Separator();
    ImGui::TextWrapped("Disable AFK timeouts from activities kicking to orbit and the title "
                       "screen.");
    ImGui::Spacing();
    changed = toggle::control("Enabled##anti_afk", settings.antiAfkEnabled) || changed;

    if (changed) {
        (void)client::player::publish(settings);
    }

    ImGui::Spacing();
    ImGui::Spacing();
    draw_season_progression();
}

} // namespace sunrise::client::ui::player
