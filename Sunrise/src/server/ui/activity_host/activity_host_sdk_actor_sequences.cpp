#include "activity_host_sdk_actor_sequences.h"

#include <array>
#include <cstdio>
#include <imgui.h>

#include "../../../state/activity_sdk/actor_sequences.h"
#include "../../activity/activity_sdk_actor_sequences.h"
#include "../../activity/activity_sdk_device_runtime.h"

namespace sunrise::server::ui::activity_host::sdk_state_pages {
namespace {
namespace sdk = state::activity_sdk;
namespace sequences = server::activity::actor_sequences;
namespace devices = server::activity::activity_sdk_devices;

sequences::Owner g_owner{};
sdk::generated_world::GeneratedWorldView g_world{};
std::uint32_t g_entry{sdk::format::kAbsentIndex};
const char* g_result = "";

/** Exact keys distinguish entries even when their readable source names coincide. */
[[nodiscard]] std::array<char, 256> label(const sdk::Catalog& catalog,
                                          const sdk::format::ActorSequenceEntry& row) noexcept {
    std::array<char, 256> output{};
    const auto symbol = catalog.string(row.symbol);
    std::snprintf(output.data(),
                  output.size(),
                  "%.*s  [0x%08X, kind %u]",
                  static_cast<int>(symbol.size()),
                  symbol.data(),
                  row.keyHash,
                  row.kind);
    return output;
}
} // namespace

/** The menu and playback route use the same extracted actor-owned catalog. */
void draw_actor_sequences(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    ImGui::SeparatorText("Animation sequence");
    const auto& held = g_world.activity_sdk_view();
    if (held.catalog != view.catalog
        || held.activityClientGeneration != view.activityClientGeneration
        || held.activityRow != view.activityRow || held.scenarioRow != view.scenarioRow
        || !state::activity::same_binding(held.binding, view.binding)) {
        if (sdk::generated_world::resolve(view, g_world)
            != sdk::generated_world::BindStatus::ready) {
            ImGui::TextDisabled("This activity has no authenticated generated-world catalog.");
            return;
        }
    }
    sequences::Owner owner{};
    if (!sequences::owner(g_world, slotRow, owner)) {
        ImGui::TextDisabled("This combatant has no exact authored actor owner.");
        return;
    }
    if (g_owner != owner) {
        g_owner = owner;
        g_entry = sdk::format::kAbsentIndex;
        g_result = "";
    }
    const auto& catalog = *view.catalog;
    const auto count = sdk::actor_sequence_count(catalog, owner.actorClassRow);
    const auto* selected = sdk::resolve_actor_sequence(catalog, owner.actorClassRow, g_entry);
    if (count == 0) {
        ImGui::TextDisabled("No unambiguous sequence entries were extracted for this actor.");
        return;
    }
    const auto selectedLabel = selected != nullptr ? label(catalog, *selected)
                                                   : std::array<char, 256>{"Choose a sequence"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 32.0F);
    if (ImGui::BeginCombo("Sequence", selectedLabel.data())) {
        for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
            const auto* const row = sdk::actor_sequence_at(catalog, owner.actorClassRow, ordinal);
            if (row == nullptr) {
                continue;
            }
            const auto rowLabel = label(catalog, *row);
            const auto index =
                static_cast<std::uint32_t>(row - catalog.actor_sequence_entries().data());
            if (ImGui::Selectable(rowLabel.data(), g_entry == index)) {
                g_entry = index;
                selected = row;
                g_result = "";
            }
        }
        ImGui::EndCombo();
    }
    if (selected != nullptr) {
        const auto name = catalog.string(selected->name);
        const auto path = catalog.string(selected->sourcePath);
        ImGui::Text("Key: 0x%08X  Kind: %u  Resource: 0x%08X",
                    selected->keyHash,
                    selected->kind,
                    selected->resourceTag);
        ImGui::Text("Table: %u  Row: %u  Offset: 0x%X",
                    selected->tableIndex,
                    selected->ordinal,
                    selected->sourceOffset);
        if (!name.empty()) {
            ImGui::Text("Name: %.*s", static_cast<int>(name.size()), name.data());
        }
        if (!path.empty()) {
            ImGui::TextWrapped("Source: %.*s", static_cast<int>(path.size()), path.data());
        }
        if (!sdk::actor_sequence_playable(*selected)) {
            ImGui::TextDisabled("This authored entry kind has no supported playback route.");
        }
    }
    ImGui::BeginDisabled(selected == nullptr || !sdk::actor_sequence_playable(*selected));
    if (ImGui::Button("Play sequence")) {
        g_result = devices::status_name(devices::play_combatant_sequence(view, slotRow, g_entry));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel sequence request")) {
        g_result = devices::status_name(devices::stop_combatant_sequence(view, slotRow));
    }
    ImGui::TextDisabled("Current actor behavior may suppress playback.");
    if (g_result[0] != '\0') {
        ImGui::Text("Sequence: %s", g_result);
    }
}

} // namespace sunrise::server::ui::activity_host::sdk_state_pages
