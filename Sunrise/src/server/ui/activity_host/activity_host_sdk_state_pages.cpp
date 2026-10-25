#include "activity_host_sdk_state_pages.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../activity/activity_sdk_device_runtime.h"
#include "../../activity/activity_sdk_lifetime_runtime.h"
#include "../../activity/activity_sdk_mission_runtime.h"
#include "../../activity/host_runtime.h"
#include "activity_host_sdk_actor_sequences.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::sdk_state_pages {
namespace {

namespace bits = middleware::encoding::bits;
namespace devices = server::activity::activity_sdk_devices;
namespace host = server::activity::host;
namespace lifetime = server::activity::activity_sdk_lifetime;
namespace mission = server::activity::activity_sdk_mission;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace mission_state = state::activity::mission;
namespace sdk = state::activity_sdk;

/** One shared table style, so every table on this page reads the same. */
constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_SizingStretchProp
                                        | ImGuiTableFlags_ScrollY;
/** Distinct state names one sensor can offer. The widest installed actor declares 139. */
constexpr std::size_t kStateCapacity = 160;
/** Search text one table keeps between frames. */
constexpr std::size_t kSearchCapacity = 64;

/** One table's search text. It filters that list only, and never changes the picked row. */
std::array<char, kSearchCapacity> g_performanceSearch{};
std::array<char, kSearchCapacity> g_engagementSearch{};
std::array<char, kSearchCapacity> g_eventSearch{};
std::array<char, kSearchCapacity> g_occupancySearch{};
std::array<char, kSearchCapacity> g_occupancyFilterSearch{};
std::array<char, kSearchCapacity> g_combatantSearch{};
std::array<char, kSearchCapacity> g_stateSearch{};
std::array<char, kSearchCapacity> g_missionSearch{};

/** Row the selected page is acting on. One page draws at a time, so one row each is enough. */
std::uint32_t g_performanceRow{sdk::format::kAbsentIndex};
std::uint32_t g_engagementRow{sdk::format::kAbsentIndex};
std::uint32_t g_eventRow{sdk::format::kAbsentIndex};
std::uint32_t g_occupancyRow{sdk::format::kAbsentIndex};
std::uint32_t g_occupancyFilterRow{sdk::format::kAbsentIndex};
std::uint32_t g_combatantRow{sdk::format::kAbsentIndex};
std::size_t g_stateRow{};

/** Values the pages hold between frames, one per editable Auth field. */
int g_engagementFlags{1};
int g_engagementRevision{1};
int g_eventState{};
float g_eventLeaveSeconds{5.0F};
std::array<char, 17> g_eventPlayerKey{"0000000000000000"};
int g_occupancyValue{};
int g_lifetimeState{};
std::array<char, 48> g_combatantChannel{};
float g_combatantValue{};
/** Last action result, so a refusal stays on screen after the frame that produced it. */
std::array<char, 96> g_result{};

/** Records one action outcome for the status line. */
void remember(const char* action, const char* status) noexcept {
    const int written = std::snprintf(g_result.data(), g_result.size(), "%s: %s", action, status);
    if (written <= 0) {
        g_result[0] = '\0';
    }
}

/** Draws the last action outcome, which is the only prose these pages print. */
void draw_result() noexcept {
    if (g_result[0] != '\0') {
        ImGui::TextDisabled("%s", g_result.data());
    }
}

/** Copies one generated string into a null-terminated label, because pack strings are not. */
void label_of(std::string_view value, std::span<char> output) noexcept {
    const std::size_t length = (std::min)(value.size(), output.size() - 1U);
    std::copy_n(value.begin(), length, output.begin());
    output[length] = char{};
}

/** Prints one durable variable value once, so the table and its search read the same text. */
void variable_text(const mission_state::VariableValue& value, std::span<char> output) noexcept {
    output[0] = char{};
    switch (value.kind) {
    case mission_state::VariableValueKind::boolean:
        (void)std::snprintf(
            output.data(), output.size(), "%s", value.booleanValue ? "true" : "false");
        return;
    case mission_state::VariableValueKind::integer:
        (void)std::snprintf(
            output.data(), output.size(), "%lld", static_cast<long long>(value.integerValue));
        return;
    case mission_state::VariableValueKind::real:
        (void)std::snprintf(output.data(), output.size(), "%.6g", value.realValue);
        return;
    case mission_state::VariableValueKind::string:
        (void)std::snprintf(output.data(),
                            output.size(),
                            "%.*s",
                            static_cast<int>(value.stringLength),
                            value.stringValue.data());
        return;
    }
}

/** ASCII case-insensitive substring match. An empty search matches every row. */
[[nodiscard]] bool contains_folded(std::string_view value, std::string_view query) noexcept {
    if (query.empty()) {
        return true;
    }
    if (query.size() > value.size()) {
        return false;
    }
    for (std::size_t start = 0; start <= value.size() - query.size(); ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < query.size(); ++index) {
            const auto left = static_cast<unsigned char>(value[start + index]);
            const auto right = static_cast<unsigned char>(query[index]);
            equal = equal && std::tolower(left) == std::tolower(right);
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

/** @return One search buffer without its trailing zero storage. */
[[nodiscard]] std::string_view
search_text(const std::array<char, kSearchCapacity>& buffer) noexcept {
    std::size_t length = 0;
    while (length < buffer.size() && buffer[length] != '\0') {
        ++length;
    }
    return {buffer.data(), length};
}

/** Draws one table's search box. The table id keeps each box distinct. */
void draw_search(std::array<char, kSearchCapacity>& buffer,
                 const char* id,
                 const char* hint) noexcept {
    std::array<char, 96> label{};
    (void)std::snprintf(label.data(), label.size(), "Search%s", id);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0F);
    (void)ImGui::InputTextWithHint(label.data(), hint, buffer.data(), buffer.size());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Filters the list. The picked row stays picked.");
    }
}

/** @return True when one search matches a slot's name, its object id, or its slot index. */
[[nodiscard]] bool slot_matches(std::string_view query,
                                std::string_view name,
                                std::string_view objectId,
                                std::uint32_t slotIndex) noexcept {
    if (query.empty()) {
        return true;
    }
    std::array<char, 16> index{};
    (void)std::snprintf(index.data(), index.size(), "%u", static_cast<unsigned>(slotIndex));
    return contains_folded(name, query) || contains_folded(objectId, query)
           || contains_folded(index.data(), query);
}

/** Rows one slot table found, and the ones its search kept. */
struct SlotCounts final {
    std::size_t total{};
    std::size_t listed{};
};

/** @return True when one slot is an exact instance of the named typed family. */
[[nodiscard]] bool exact_slot(const sdk::format::Slot& slot,
                              std::uint32_t slotType,
                              std::uint32_t authSchema) noexcept {
    return slot.slotType == slotType && slot.authSchema == authSchema
           && (slot.flags & sdk::format::kSlotSchemaJoinExact) != 0;
}

/**
 * Draws one searchable, selectable table of every exact slot of one family in the scenario.
 * @param search Search text for this table, drawn above it.
 * @param selected Row the table is on, updated on a pick.
 * @return Exact rows found, and the ones the search kept.
 */
[[nodiscard]] SlotCounts draw_slot_table(const sdk::BoundView& view,
                                         std::uint32_t slotType,
                                         std::uint32_t authSchema,
                                         const char* id,
                                         std::array<char, kSearchCapacity>& search,
                                         std::uint32_t& selected) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    const sdk::format::Scenario* const scenario = sdk::bound_scenario(view);
    if (scenario == nullptr) {
        return {};
    }
    const auto slots = catalog.slots();
    const auto objects = catalog.objects();
    draw_search(search, id, "name, object or slot");
    const std::string_view query = search_text(search);
    SlotCounts counts{};
    if (!ImGui::BeginTable(id, 3, kTableFlags, table_layout::size(8))) {
        return counts;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("object", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("slot");
    table_layout::frozen_headers();
    for (const sdk::format::Occurrence& occurrence :
         sdk::scenario_occurrences(catalog, *scenario)) {
        if (occurrence.objectIndex >= objects.size()) {
            continue;
        }
        const sdk::format::Object& object = objects[occurrence.objectIndex];
        for (const sdk::format::Slot& slot : sdk::object_slots(catalog, object)) {
            if (!exact_slot(slot, slotType, authSchema)) {
                continue;
            }
            ++counts.total;
            const std::string_view name = catalog.string(slot.name);
            const std::string_view objectId = catalog.string(object.id);
            if (!slot_matches(query, name, objectId, slot.slotIndex)) {
                continue;
            }
            const auto slotRow = static_cast<std::uint32_t>(&slot - slots.data());
            ++counts.listed;
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(slotRow));
            std::array<char, 96> label{};
            label_of(name.empty() ? std::string_view("unnamed") : name, label);
            if (table_layout::selectable(label.data(), slotRow == selected)) {
                selected = slotRow;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(objectId.data(), objectId.data() + objectId.size());
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned>(slot.slotIndex));
        }
    }
    ImGui::EndTable();
    return counts;
}

/** @return True when the page has a live catalog and a picked row. */
[[nodiscard]] bool ready(const sdk::BoundView& view, std::uint32_t selected) noexcept {
    return view.catalog != nullptr && selected != sdk::format::kAbsentIndex
           && selected < view.catalog->slots().size();
}

/** Folds one channel name the way the client's channel registry keys it. */
[[nodiscard]] std::uint32_t fnv1(std::string_view name) noexcept {
    if (name.empty()) {
        return 0;
    }
    std::uint32_t value = 0x811C'9DC5U;
    for (const char character : name) {
        value *= 0x0100'0193U;
        value ^= static_cast<unsigned char>(character);
    }
    return value;
}

/** Reads the sixteen hex digits of the watched player key. */
[[nodiscard]] bool player_key(std::uint64_t& output) noexcept {
    output = 0;
    for (const char digit : std::string_view(g_eventPlayerKey.data())) {
        const unsigned value =
            digit >= '0' && digit <= '9'
                ? static_cast<unsigned>(digit - '0')
                : (digit >= 'a' && digit <= 'f'
                       ? static_cast<unsigned>(digit - 'a') + 10U
                       : (digit >= 'A' && digit <= 'F' ? static_cast<unsigned>(digit - 'A') + 10U
                                                       : 16U));
        if (value == 16U) {
            return false;
        }
        output = (output << 4U) | value;
    }
    return output != 0;
}

} // namespace

/** Draws the performance slots of the bound view and their per-state rows. */
void draw_performances(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return;
    }
    const SlotCounts counts = draw_slot_table(view,
                                              sdk::format::kPerformanceSlotType,
                                              sdk::format::kPerformanceAuthSchema,
                                              "##sdk_performance_slots",
                                              g_performanceSearch,
                                              g_performanceRow);
    if (counts.total == 0) {
        ImGui::TextDisabled("This scenario has no performance sensor.");
        return;
    }
    if (counts.listed == 0) {
        ImGui::TextDisabled("No sensor matches the search.");
    }
    if (!ready(view, g_performanceRow)) {
        ImGui::TextDisabled("Pick a sensor.");
        return;
    }
    const sdk::format::Scenario* const scenario = sdk::bound_scenario(view);
    std::array<std::uint32_t, kStateCapacity> names{};
    const std::size_t count =
        scenario == nullptr
            ? 0
            : sdk::performance_state_names(*view.catalog, *scenario, g_performanceRow, names);
    if (count == 0) {
        ImGui::TextDisabled("This sensor reaches no actor that declares a state.");
        return;
    }
    if (g_stateRow >= count) {
        g_stateRow = 0;
    }
    std::array<char, 16> preview{};
    (void)std::snprintf(
        preview.data(), preview.size(), "%08X", static_cast<unsigned>(names[g_stateRow]));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0F);
    if (ImGui::BeginCombo("State", preview.data())) {
        for (std::size_t row = 0; row < count; ++row) {
            std::array<char, 16> label{};
            (void)std::snprintf(
                label.data(), label.size(), "%08X", static_cast<unsigned>(names[row]));
            if (ImGui::Selectable(label.data(), row == g_stateRow)) {
                g_stateRow = row;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Start")) {
        const mission::SceneStatus status =
            mission::play_performance_slot(view, g_performanceRow, names[g_stateRow]);
        remember("start", mission::status_name(status));
    }
    draw_result();
}

/** Draws the engagement slots of the bound view. */
void draw_engagement(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return;
    }
    const SlotCounts counts = draw_slot_table(view,
                                              scriptable_auth::kType70SlotType,
                                              scriptable_auth::kType70Schema,
                                              "##sdk_engagement_slots",
                                              g_engagementSearch,
                                              g_engagementRow);
    if (counts.total == 0) {
        ImGui::TextDisabled("This scenario has no engagement sensor.");
        return;
    }
    if (counts.listed == 0) {
        ImGui::TextDisabled("No sensor matches the search.");
    }
    if (!ready(view, g_engagementRow)) {
        ImGui::TextDisabled("Pick a sensor.");
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0F);
    ImGui::InputInt("Flags", &g_engagementFlags);
    g_engagementFlags = (std::clamp)(g_engagementFlags, 0, 31);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0F);
    ImGui::InputInt("Revision", &g_engagementRevision);
    g_engagementRevision = (std::clamp)(g_engagementRevision, 0, 32'767);
    ImGui::SameLine();
    if (ImGui::Button("Send")) {
        scriptable_auth::Type70Preset preset{};
        preset.flags = static_cast<std::uint8_t>(g_engagementFlags);
        preset.revision = static_cast<std::int16_t>(g_engagementRevision);
        std::array<std::byte, scriptable_auth::kType70ByteCount> body{};
        std::size_t written = 0;
        if (!scriptable_auth::encode_type70(preset, body, written) || written != body.size()) {
            remember("send", "encoder refused");
        } else {
            const devices::Status status = devices::apply_auth_slot(
                view, g_engagementRow, body, scriptable_auth::kType70BitCount);
            remember("send", devices::status_name(status));
        }
    }
    ImGui::TextDisabled("A higher revision empties the participant list the client reports.");
    draw_result();
}

/** Draws the public event slots of the bound view. */
void draw_public_events(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return;
    }
    const SlotCounts counts = draw_slot_table(view,
                                              scriptable_auth::kType71SlotType,
                                              scriptable_auth::kType71Schema,
                                              "##sdk_event_slots",
                                              g_eventSearch,
                                              g_eventRow);
    if (counts.total == 0) {
        ImGui::TextDisabled("This scenario has no public-event sensor.");
        return;
    }
    if (counts.listed == 0) {
        ImGui::TextDisabled("No sensor matches the search.");
    }
    if (!ready(view, g_eventRow)) {
        ImGui::TextDisabled("Pick a sensor.");
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0F);
    ImGui::InputInt("State", &g_eventState);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0F);
    ImGui::InputFloat("Leave s", &g_eventLeaveSeconds, 0.0F, 0.0F, "%.1f");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0F);
    ImGui::InputText("Player key", g_eventPlayerKey.data(), g_eventPlayerKey.size());
    const sdk::Catalog& catalog = *view.catalog;
    const auto slots = catalog.slots();
    const sdk::format::Slot& area = slots[g_eventRow];
    const auto objects = catalog.objects();
    if (ImGui::Button("Send")) {
        std::uint64_t identity = 0;
        if (!player_key(identity)) {
            remember("send", "player key is not sixteen hex digits");
        } else if (area.objectIndex >= objects.size()) {
            remember("send", "slot has no object");
        } else {
            scriptable_auth::Type71Body body{};
            body.state = g_eventState;
            body.playerIdentity = identity;
            // The sensor watches its own object's zone list until a separate area is chosen.
            body.areaRegistryKey = objects[area.objectIndex].objectKey;
            body.areaSlotType = static_cast<std::uint8_t>(area.slotType);
            body.areaSlotIndex = static_cast<std::uint16_t>(area.slotIndex);
            body.leaveSeconds = g_eventLeaveSeconds;
            std::array<std::byte, scriptable_auth::kType71ByteCount> encoded{};
            std::size_t written = 0;
            if (!scriptable_auth::encode_type71(body, encoded, written)
                || written != encoded.size()) {
                remember("send", "encoder refused");
            } else {
                const devices::Status status = devices::apply_auth_slot(
                    view, g_eventRow, encoded, scriptable_auth::kType71BitCount);
                remember("send", devices::status_name(status));
            }
        }
    }
    ImGui::TextDisabled("Sense 1 means that player has left. Only a new key re-arms it.");
    draw_result();
}

/** Draws the occupancy slots of the bound view and their conditions. */
void draw_occupancy(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return;
    }
    const SlotCounts counts = draw_slot_table(view,
                                              sdk::format::kOccupancySlotType,
                                              sdk::format::kOccupancyAuthSchema,
                                              "##sdk_occupancy_slots",
                                              g_occupancySearch,
                                              g_occupancyRow);
    if (counts.total == 0) {
        ImGui::TextDisabled("This scenario has no occupancy condition.");
        return;
    }
    if (counts.listed == 0) {
        ImGui::TextDisabled("No condition matches the search.");
    }
    if (!ready(view, g_occupancyRow)) {
        ImGui::TextDisabled("Pick a condition.");
        return;
    }
    ImGui::TextDisabled("Filter");
    const SlotCounts filters = draw_slot_table(view,
                                               scriptable_auth::kType34SlotType,
                                               scriptable_auth::kType34Schema,
                                               "##sdk_occupancy_filters",
                                               g_occupancyFilterSearch,
                                               g_occupancyFilterRow);
    if (filters.total != 0 && filters.listed == 0) {
        ImGui::TextDisabled("No filter matches the search.");
    }
    if (filters.total == 0 || !ready(view, g_occupancyFilterRow)) {
        ImGui::TextDisabled("Pick the object filter this condition counts.");
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0F);
    ImGui::InputInt("Value", &g_occupancyValue);
    ImGui::SameLine();
    if (ImGui::Button("Send")) {
        const sdk::Catalog& catalog = *view.catalog;
        const sdk::format::Slot& filter = catalog.slots()[g_occupancyFilterRow];
        const auto objects = catalog.objects();
        if (filter.objectIndex >= objects.size()) {
            remember("send", "filter slot has no object");
        } else {
            std::array<std::byte, 11> body{};
            bits::Writer writer(body);
            const std::uint32_t encoded =
                std::bit_cast<std::uint32_t>(g_occupancyValue) + 0x8000'0000U;
            const bool built = writer.write(objects[filter.objectIndex].objectKey, 32)
                               && writer.write(filter.slotType + 1U, 7)
                               && writer.write(filter.slotIndex + 32'768U, 16)
                               && writer.write(encoded, 32);
            if (!built) {
                remember("send", "encoder refused");
            } else {
                const devices::Status status =
                    devices::apply_auth_slot(view, g_occupancyRow, body, 87);
                remember("send", devices::status_name(status));
            }
        }
    }
    ImGui::TextDisabled("The client copies the value to Sense. It compares nothing.");
    draw_result();
}

/** Draws the combatant slots of the bound view. */
void draw_combatants(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return;
    }
    const SlotCounts counts = draw_slot_table(view,
                                              scriptable_auth::kType2SlotType,
                                              scriptable_auth::kType2Schema,
                                              "##sdk_combatant_slots",
                                              g_combatantSearch,
                                              g_combatantRow);
    if (counts.total == 0) {
        ImGui::TextDisabled("This scenario has no combatant slot.");
        return;
    }
    if (counts.listed == 0) {
        ImGui::TextDisabled("No combatant matches the search.");
    }
    if (!ready(view, g_combatantRow)) {
        ImGui::TextDisabled("Pick a combatant.");
        return;
    }
    if (ImGui::Button("Bind to squad")) {
        remember("bind",
                 devices::status_name(devices::bind_combatant_to_squad(view, g_combatantRow)));
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0F);
    ImGui::InputText("Channel", g_combatantChannel.data(), g_combatantChannel.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0F);
    ImGui::InputFloat("Value", &g_combatantValue, 0.0F, 0.0F, "%.3f");
    ImGui::SameLine();
    if (ImGui::Button("Retain")) {
        const std::uint32_t hash = fnv1(std::string_view(g_combatantChannel.data()));
        remember("retain",
                 hash == 0 ? "channel name is empty"
                           : devices::status_name(devices::set_combatant_channel(
                                 view, g_combatantRow, hash, g_combatantValue)));
    }
    ImGui::TextDisabled("The value is kept for the next attach or restore.");
    draw_actor_sequences(view, g_combatantRow);
    draw_result();
}

/** Draws the authored states of the bound view and which one is selected. */
void draw_states(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const sdk::format::Scenario* const scenario = sdk::bound_scenario(view);
    if (scenario == nullptr) {
        ImGui::TextDisabled("No bound scenario.");
        return;
    }
    const auto states = catalog.states();
    draw_search(g_stateSearch, "##sdk_states", "state, region or bubble");
    const std::string_view query = search_text(g_stateSearch);
    if (!ImGui::BeginTable("##sdk_states", 4, kTableFlags, table_layout::size(10))) {
        return;
    }
    ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("region");
    ImGui::TableSetupColumn("bubble");
    ImGui::TableSetupColumn("action");
    table_layout::frozen_headers();
    for (std::size_t offset = 0; offset < scenario->states.count; ++offset) {
        const std::size_t row = scenario->states.first + offset;
        if (row >= states.size()) {
            break;
        }
        const sdk::format::State& state = states[row];
        const std::int32_t region =
            static_cast<std::int32_t>(state.sliceSetIndex + state.stateOrdinal);
        const std::string_view stateId = catalog.string(state.id);
        std::array<char, 32> numbers{};
        (void)std::snprintf(numbers.data(),
                            numbers.size(),
                            "%d %u",
                            region,
                            static_cast<unsigned>(state.bubbleIndex));
        if (!contains_folded(stateId, query) && !contains_folded(numbers.data(), query)) {
            continue;
        }
        table_layout::next_row();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(stateId.data(), stateId.data() + stateId.size());
        ImGui::TableNextColumn();
        ImGui::Text("%d", region);
        ImGui::TableNextColumn();
        ImGui::Text("%u", static_cast<unsigned>(state.bubbleIndex));
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(row));
        if (ImGui::SmallButton("Select")) {
            mission::Snapshot plan{};
            remember("select", mission::status_name(mission::select_state(view, region, {}, plan)));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Picking a state the client does not hold arms a host teleport.");
    draw_result();
}

/** Draws the durable mission state of the bound view. */
void draw_mission_state(const sdk::BoundView& view) noexcept {
    mission_state::Snapshot snapshot{};
    if (!mission_state::state_snapshot(view.binding, snapshot)) {
        ImGui::TextDisabled("This session holds no mission record.");
        return;
    }
    ImGui::Text("Revision %llu  phase %u",
                static_cast<unsigned long long>(snapshot.activityStateRevision),
                static_cast<unsigned>(snapshot.state.phase));
    draw_search(g_missionSearch, "##sdk_mission_state", "variable, timer or value");
    const std::string_view query = search_text(g_missionSearch);
    if (ImGui::BeginTable("##sdk_mission_variables", 2, kTableFlags, table_layout::size(8))) {
        ImGui::TableSetupColumn("variable", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        table_layout::frozen_headers();
        for (const auto& variable : snapshot.state.variables) {
            if (variable.key.length == 0) {
                continue;
            }
            const std::string_view key(variable.key.bytes.data(), variable.key.length);
            std::array<char, mission_state::kVariableStringByteCapacity + 8> value{};
            variable_text(variable.value, value);
            if (!contains_folded(key, query) && !contains_folded(value.data(), query)) {
                continue;
            }
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(key.data(), key.data() + key.size());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.data());
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##sdk_mission_timers", 3, kTableFlags, table_layout::size(6))) {
        ImGui::TableSetupColumn("timer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("deadline tick");
        ImGui::TableSetupColumn("sequence");
        table_layout::frozen_headers();
        for (const auto& timer : snapshot.state.timers) {
            if (timer.key.length == 0) {
                continue;
            }
            const std::string_view key(timer.key.bytes.data(), timer.key.length);
            std::array<char, 48> numbers{};
            (void)std::snprintf(numbers.data(),
                                numbers.size(),
                                "%llu %llu",
                                static_cast<unsigned long long>(timer.deadlineTick),
                                static_cast<unsigned long long>(timer.sequence));
            if (!contains_folded(key, query) && !contains_folded(numbers.data(), query)) {
                continue;
            }
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(key.data(), key.data() + key.size());
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(timer.deadlineTick));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(timer.sequence));
        }
        ImGui::EndTable();
    }
}

/** Draws the lifetime slot of the bound view and its state control. */
void draw_lifetime(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No bound catalog.");
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0F);
    ImGui::InputInt("State", &g_lifetimeState);
    g_lifetimeState =
        (std::clamp)(g_lifetimeState, 0, static_cast<int>(host::kMaximumLifetimeState));
    ImGui::SameLine();
    const auto state = static_cast<std::uint8_t>(g_lifetimeState);
    ImGui::TextDisabled("%s", devices::status_name(lifetime::availability(view, state)));
    if (ImGui::Button("Set")) {
        remember("set", devices::status_name(lifetime::set(view, state)));
    }
    draw_result();
}

} // namespace sunrise::server::ui::activity_host::sdk_state_pages
