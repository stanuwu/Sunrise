/**
 * The session overlay. One row per instance the player is in.
 * A row names the region, its group session, its activity host, its channel, how far its join has
 * got and whether its player row has published.
 */

#include "ui_hud_session_overlay.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>

#include "../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../server/gameplay/group/group_host.h"
#include "../../../../server/gameplay/group/group_host_sessions.h"
#include "../../../../server/gameplay/peer/peer_transport.h"
#include "../../../../state/gameplay/definition.h"

namespace sunrise::core::ui::hud::overlays::session {
namespace {

namespace group = server::gameplay::group;
namespace peer = server::gameplay::peer;
namespace tables = middleware::content::packages::tables;

/** Instances one snapshot reads. The host table holds no more than this. */
constexpr std::size_t kRowCapacity = 8;
/** Admitted peers one snapshot reads. */
constexpr std::size_t kAdmittedCapacity = 8;
/** Columns of the instance table, in draw order. */
constexpr int kColumnCount = 7;
/** Shown while this host serves no instance. */
constexpr char kNoInstance[] = "no session instances";
/** Shown for a row no advertisement gave a region. */
constexpr char kNoBubble[] = "-";

/** Channel stage names, in PeerStage order. */
constexpr std::array<const char*, 6> kChannelStages{
    "absent", "allocated", "teardown", "connecting", "establishing", "connected"};

/** @param stage Link stage. @return Its name, or the absent one for a value out of range. */
[[nodiscard]] const char* channel_name(state::gameplay::PeerStage stage) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    return index < kChannelStages.size() ? kChannelStages[index] : kChannelStages[0];
}

/**
 * The two state words for one instance, in the order the join reaches them.
 * `hosted` is the end of the join ladder: the peer builds no activity client until it holds the
 * activity-host parameter, so a row short of it is still waiting on this host.
 */
struct InstanceState {
    const char* join{"unjoined"};
    const char* player{"-"};
};

/**
 * Reads how far one instance has got.
 * @param admitted Admitted rows from the snapshot.
 * @param count Admitted rows in use.
 * @param sessionId Group session the row names.
 * @return Both state words, or the absent pair when no record holds that session.
 */
[[nodiscard]] InstanceState
instance_state(const std::array<group::AdmittedRow, kAdmittedCapacity>& admitted,
               std::size_t count,
               std::uint64_t sessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        const group::AdmittedRow& row = admitted[index];
        if (row.sessionId != sessionId) {
            continue;
        }
        InstanceState state{};
        if (!row.joinComplete) {
            state.join = "joining";
        } else {
            state.join = row.activityHostPublished ? "hosted" : "admitted";
        }
        // A peer that has asked for no player owes nothing, which is not the same as one whose
        // player row the reliable queue has refused so far.
        if (!row.hasPlayer) {
            state.player = "none";
        } else {
            state.player = row.playerPublished ? "published" : "owed";
        }
        return state;
    }
    return {};
}

/** Draws one cell of hexadecimal identity. @param value Session id, or zero when there is none. */
void draw_session_cell(std::uint64_t value) noexcept {
    if (value == 0) {
        ImGui::TextDisabled("pending");
        return;
    }
    ImGui::Text("%016llX", static_cast<unsigned long long>(value));
}

/** Draws the region and bubble cells. @param region Region the advertisement named, or -1. */
void draw_region_cells(std::int32_t region) noexcept {
    if (region < 0) {
        ImGui::TextDisabled("%s", kNoBubble);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", kNoBubble);
        return;
    }
    ImGui::Text("%d", static_cast<int>(region));
    ImGui::TableNextColumn();
    ImGui::Text("%u", static_cast<unsigned>(region) / tables::kSliceSetIndexFactor);
}

} // namespace

/** Draws the session instance table inside the overlay window the stack has already started. */
void draw() noexcept {
    std::array<group::HostSessionRow, kRowCapacity> rows{};
    std::size_t rowCount = 0;
    group::snapshot_host_sessions(rows, rowCount);
    // A host session outlives the instance on purpose: the peer may rotate back into a region it
    // never left, and a fresh id there is a hard error. So the link is what says it is still live.
    std::array<state::gameplay::PeerStage, kRowCapacity> stages{};
    std::size_t live = 0;
    for (std::size_t index = 0; index < rowCount; ++index) {
        if (peer::link_stage(rows[index].groupSessionId, stages[live])) {
            rows[live] = rows[index];
            ++live;
        }
    }
    if (live == 0) {
        ImGui::TextDisabled("%s", kNoInstance);
        return;
    }
    std::array<group::AdmittedRow, kAdmittedCapacity> admitted{};
    std::size_t admittedCount = 0;
    group::snapshot_admitted(admitted, admittedCount);

    if (!ImGui::BeginTable("##sunrise_hud_session_table", kColumnCount)) {
        return;
    }
    ImGui::TableSetupColumn("region");
    ImGui::TableSetupColumn("bubble");
    ImGui::TableSetupColumn("group session");
    ImGui::TableSetupColumn("activity host");
    ImGui::TableSetupColumn("channel");
    ImGui::TableSetupColumn("join");
    ImGui::TableSetupColumn("player");
    ImGui::TableHeadersRow();
    for (std::size_t index = 0; index < live; ++index) {
        const group::HostSessionRow& row = rows[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_region_cells(row.regionIndex);
        ImGui::TableNextColumn();
        draw_session_cell(row.groupSessionId);
        ImGui::TableNextColumn();
        draw_session_cell(row.hostSessionId);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(channel_name(stages[index]));
        const InstanceState state = instance_state(admitted, admittedCount, row.groupSessionId);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(state.join);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(state.player);
    }
    ImGui::EndTable();
}

} // namespace sunrise::core::ui::hud::overlays::session
