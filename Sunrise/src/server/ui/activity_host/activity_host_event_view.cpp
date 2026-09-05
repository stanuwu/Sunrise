#include "activity_host_event_view.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../core/ui/components/card/ui_card_component.h"
#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../middleware/bap/activity_message/wire_schema/activity_wire_schema.h"
#include "../../activity/host_runtime.h"
#include "activity_host_incident_editor.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::event_view {
namespace {

namespace host = server::activity::host;
namespace card = core::ui::components::card;
namespace section = core::ui::components::section;
namespace wire_schema = middleware::bap::activity_message::wire_schema;
namespace sense_update = middleware::bap::activity_message::sense_update;

/** Activity message types with decoded detail rows in this view. */
constexpr std::uint32_t kSenseMessageType = 6;
constexpr std::uint32_t kIncidentMessageType = 19;
constexpr std::uint32_t kAuthoritativeMessageType = 22;

/** Shared fixed-width table style for both histories. */
constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

bool g_showAllClientMessages{true};
bool g_clientMessagesNewestFirst{true};
bool g_coalescePatchEpoch{true};
int g_clientMessageTypeFilter{-1};
std::uint64_t g_selectedClientMessageSequence{};

/** @return True when compact ingress identity names the selected activity generation. */
[[nodiscard]] bool same_binding(const host::ClientMessageBinding& left,
                                const state::activity::SessionBinding& right) noexcept {
    return left.sessionId == right.sessionId && left.createdRevision == right.createdRevision;
}

/** @return True when one ingress row passes the current generation and type filters. */
[[nodiscard]] bool
client_message_visible(const host::ClientMessageRecord& record,
                       const state::activity::SessionBinding* selected) noexcept {
    return (g_showAllClientMessages
            || (selected != nullptr && same_binding(record.binding, *selected)))
           && (g_clientMessageTypeFilter < 0
               || record.messageType == static_cast<std::uint32_t>(g_clientMessageTypeFilter));
}

/** Finds one retained ingress row by sequence. */
[[nodiscard]] const host::ClientMessageRecord*
find_client_message(const host::DiagnosticsSnapshot& snapshot, std::uint64_t sequence) noexcept {
    for (std::size_t index = snapshot.clientMessageCount; index != 0; --index) {
        if (snapshot.clientMessages[index - 1].sequence == sequence) {
            return &snapshot.clientMessages[index - 1];
        }
    }
    return nullptr;
}

/** Formats one schema-selected msg-6 scalar without assigning a semantic label. */
void format_value(const sense_update::DecodedValue& value, std::array<char, 96>& output) noexcept {
    if (!value.present) {
        (void)std::snprintf(output.data(), output.size(), "absent");
        return;
    }
    switch (value.kind) {
    case sense_update::ValueKind::boolean:
        (void)std::snprintf(
            output.data(), output.size(), "%s", value.unsignedValue != 0 ? "true" : "false");
        return;
    case sense_update::ValueKind::signedInteger:
        (void)std::snprintf(
            output.data(), output.size(), "%lld", static_cast<long long>(value.signedValue));
        return;
    case sense_update::ValueKind::real32:
        (void)std::snprintf(output.data(), output.size(), "%.6g", value.realValue);
        return;
    case sense_update::ValueKind::unsignedInteger:
        (void)std::snprintf(output.data(),
                            output.size(),
                            "%llu",
                            static_cast<unsigned long long>(value.unsignedValue));
        return;
    }
}

/** Draws numeric SDK joins and recursive scalar values for one msg-6 sidecar. */
void draw_sense_values(const host::ClientMessageDetail& detail) noexcept {
    const sense_update::DecodedPacket& packet = detail.sense;
    for (std::size_t objectIndex = 0; objectIndex < packet.objectCount; ++objectIndex) {
        const sense_update::DecodedObject& object = packet.objects[objectIndex];
        ImGui::PushID(static_cast<int>(objectIndex));
        std::array<char, 192> label{};
        (void)std::snprintf(label.data(),
                            label.size(),
                            "0x%08X / type %u / index %u##sense_object",
                            object.registryKey,
                            static_cast<unsigned>(object.slotType),
                            static_cast<unsigned>(object.slotIndex));
        if (ImGui::TreeNodeEx(label.data(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            ImGui::Text(
                "object tag: 0x%08X; SDK object row: %u", object.objectTag, object.objectRow);
            ImGui::Text("slot row: %u; Sense schema: 0x%08X; schema row: %u",
                        object.slotRow,
                        object.senseSchema,
                        object.schemaRow);
            ImGui::Text("status: %s; delta: %u bits",
                        sense_update::object_status_name(object.status),
                        object.deltaBits);
            if (object.hasGeneration) {
                ImGui::Text("generation + 1: %u", object.generationPlusOne);
            }
            const std::size_t first = object.firstValue;
            const std::size_t count =
                first <= packet.valueCount && object.valueCount <= packet.valueCount - first
                    ? object.valueCount
                    : 0;
            if (count != 0
                && ImGui::BeginTable("##sense_values",
                                     2,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                         | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("SDK field", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
                table_layout::frozen_headers();
                for (std::size_t index = 0; index < count; ++index) {
                    const sense_update::DecodedValue& value = packet.values[first + index];
                    table_layout::next_row();
                    ImGui::TableNextColumn();
                    ImGui::Text("schema %u / field %u[%u]",
                                value.schemaRow,
                                value.fieldRow,
                                value.occurrence);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("field ordinal %u; wire bit %u; width %u",
                                          static_cast<unsigned>(value.fieldOrdinal),
                                          value.bitOffset,
                                          static_cast<unsigned>(value.width));
                    }
                    ImGui::TableNextColumn();
                    std::array<char, 96> formatted{};
                    format_value(value, formatted);
                    ImGui::TextUnformatted(formatted.data());
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (packet.objectsTruncated || packet.valuesTruncated) {
        ImGui::TextDisabled("More rows were decoded. They are not kept.");
    }
}

/** Draws one event's bounded details without treating the opaque tail as a sensor value. */
void draw_event_details(const host::Event& event) noexcept {
    if (event.kind == host::EventKind::senseUpdate) {
        ImGui::Text("groups %u: %u decoded, %u skipped",
                    event.groupsSeen,
                    event.groupsDecoded,
                    event.groupsSkipped);
        ImGui::Text("objects %u: %u decoded", event.objectsSeen, event.objectsDecoded);
        return;
    }
    if (event.kind == host::EventKind::clientStateChanged) {
        ImGui::Text("State revision %llu, membership revision %u",
                    static_cast<unsigned long long>(event.activityStateRevision),
                    event.membershipRevision);
        if (event.clientStateHasRegion) {
            ImGui::Text(
                "region %d, slice-set hash 0x%08X", event.regionIndex, event.regionSliceSetHash);
        }
        ImGui::Text("held region %d", event.heldRegionIndex);
        if (event.clientStateHasSpawn) {
            ImGui::Text("spawn state %d", static_cast<int>(event.spawnState));
        }
        if (event.clientStateHasTeleport) {
            ImGui::Text("teleport state %d, slice set %d / 0x%08X",
                        static_cast<int>(event.teleportState),
                        event.teleportSliceSetIndex,
                        event.teleportSliceSetHash);
        }
        return;
    }
    if (event.kind == host::EventKind::entitySlotsRequested) {
        ImGui::Text("Requested simulation entity slots %d", event.requestedEntitySlots);
        return;
    }
    if (event.kind == host::EventKind::incidentReceived
        || event.kind == host::EventKind::incidentQueued
        || event.kind == host::EventKind::incidentTransportStaged
        || event.kind == host::EventKind::incidentCanceled
        || event.kind == host::EventKind::incidentRefused) {
        ImGui::Text("unmapped target, %u B", event.incidentPayloadBytes);
        return;
    }
    if (event.kind == host::EventKind::scriptableOverrideCommitted) {
        ImGui::TextUnformatted("ClientRef auth body kept");
        return;
    }
    if (event.kind == host::EventKind::scriptableOverrideTransportStaged) {
        ImGui::TextUnformatted("ClientRef counter committed");
        return;
    }
    if (event.kind == host::EventKind::scriptableOverrideCanceled) {
        ImGui::TextUnformatted("ClientRef body cleared. The counter is unchanged.");
        return;
    }
    ImGui::TextUnformatted("presentation state");
}

/** @return The event's transport or operator result for the result column. */
[[nodiscard]] const char* event_result(const host::Event& event) noexcept {
    switch (event.kind) {
    case host::EventKind::senseUpdate:
        return sense_update::decode_status_name(event.senseDecodeStatus);
    case host::EventKind::clientStateChanged:
        return "committed";
    case host::EventKind::clientMessageReceived:
        return host::client_message_status_name(event.clientMessageStatus);
    case host::EventKind::authStateCommitted:
        return "pending";
    case host::EventKind::authStateTransportStaged:
        return "transport staged";
    case host::EventKind::authStateCanceled:
        return "canceled";
    case host::EventKind::incidentReceived:
        return "captured";
    case host::EventKind::incidentQueued:
        return "pending";
    case host::EventKind::incidentTransportStaged:
        return "transport staged";
    case host::EventKind::incidentCanceled:
        return "canceled";
    case host::EventKind::incidentRefused:
        return "refused";
    case host::EventKind::scriptableOverrideCommitted:
        return "typed override queued";
    case host::EventKind::scriptableOverrideTransportStaged:
        return "transport staged";
    case host::EventKind::scriptableOverrideCanceled:
        return "canceled";
    case host::EventKind::operatorRefused:
        return "refused";
    }
    return "unknown";
}

/** Draws safe typed msg-22 fields already owned by the membership transaction. */
void draw_authoritative_detail(const host::ClientMessageRecord& record) noexcept {
    if (!record.hasAuthoritative) {
        ImGui::TextDisabled("decoded fields unavailable");
        return;
    }
    const auto& update = record.authoritative;
    if (update.hasTransitionToken) {
        ImGui::TextUnformatted("Transition");
    }
    if (update.hasSpawn) {
        ImGui::TextUnformatted("Spawn update");
    }
    if (update.hasTeleport) {
        ImGui::TextUnformatted("Teleport update");
    }
    if (update.hasRegion) {
        ImGui::TextUnformatted("Region update");
    }
    if (!update.hasTransitionToken && !update.hasSpawn && !update.hasTeleport
        && !update.hasRegion) {
        ImGui::TextDisabled("no reflected fields");
    }
}

/** @return True while the ingress row has a retained parsed msg-19 body. */
[[nodiscard]] bool incident_retained(const host::DiagnosticsSnapshot& snapshot,
                                     std::uint64_t ingressSequence) noexcept {
    return std::any_of(snapshot.incidents.begin(),
                       snapshot.incidents.begin()
                           + static_cast<std::ptrdiff_t>(snapshot.incidentCount),
                       [ingressSequence](const host::IncidentRecord& incident) noexcept {
                           return incident.clientMessageSequence == ingressSequence;
                       });
}

/** Draws only parser facts and already-decoded safe fields for one ingress row. */
void draw_client_message_detail(const host::ClientMessageRecord& record,
                                const host::DiagnosticsSnapshot& snapshot,
                                const host::ClientMessageDetail* detail = nullptr) noexcept {
    const std::uint64_t payloadBits = static_cast<std::uint64_t>(record.payloadBytes) * 8ULL;
    if (record.messageType == kSenseMessageType) {
        if (detail == nullptr || !detail->hasSenseDecode) {
            ImGui::TextDisabled("typed decode not retained");
            return;
        }
        const sense_update::DecodedPacket& sense = detail->sense;
        ImGui::Text("%s; %zu/%llu bits",
                    sense_update::decode_status_name(sense.status),
                    sense.bitsConsumed,
                    static_cast<unsigned long long>(payloadBits));
        ImGui::Text("groups %u: %u decoded, %u skipped; objects %u: %u decoded",
                    sense.groupsSeen,
                    sense.groupsDecoded,
                    sense.groupsSkipped,
                    sense.objectsSeen,
                    sense.objectsDecoded);
        draw_sense_values(*detail);
        return;
    }
    if (record.messageType == kIncidentMessageType
        && record.status == host::ClientMessageStatus::outerDecoded) {
        ImGui::TextUnformatted(incident_retained(snapshot, record.sequence)
                                   ? "outer fields and payload kept"
                                   : "payload not kept");
        ImGui::TextDisabled("the target picks the schema. It is not decoded.");
        return;
    }
    if (record.messageType == kAuthoritativeMessageType) {
        draw_authoritative_detail(record);
        return;
    }
    if (record.consumedBits != 0) {
        ImGui::Text("typed walk %u/%llu bits",
                    record.consumedBits,
                    static_cast<unsigned long long>(payloadBits));
        return;
    }
    ImGui::TextDisabled("metadata only");
}

/** Draws a packet-name filter backed by the binary's own table. */
void draw_message_filter() noexcept {
    const char* preview = "All messages";
    if (g_clientMessageTypeFilter >= 0) {
        preview = host::client_message_name(static_cast<std::uint32_t>(g_clientMessageTypeFilter));
    }
    ImGui::SetNextItemWidth(260.0F);
    if (!ImGui::BeginCombo("Message", preview)) {
        return;
    }
    if (ImGui::Selectable("All messages", g_clientMessageTypeFilter < 0)) {
        g_clientMessageTypeFilter = -1;
    }
    for (const wire_schema::MessageDescriptor& message : wire_schema::messages()) {
        if (!message.clientSends) {
            continue;
        }
        const bool selected = g_clientMessageTypeFilter == static_cast<int>(message.id);
        ImGui::PushID(static_cast<int>(message.id));
        if (ImGui::Selectable(message.name.data(), selected)) {
            g_clientMessageTypeFilter = static_cast<int>(message.id);
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

/** Draws one selected incoming packet with semantic fields first. */
void draw_selected_client_message(const host::DiagnosticsSnapshot& snapshot) noexcept {
    const host::ClientMessageRecord* const record =
        find_client_message(snapshot, g_selectedClientMessageSequence);
    if (record == nullptr) {
        ImGui::TextDisabled("Select an incoming packet.");
        return;
    }
    ImGui::SeparatorText(host::client_message_name(record->messageType));
    host::ClientMessageDetail detail{};
    const bool hasDetail = host::client_message_detail(record->sequence, detail);
    const bool specialized = record->messageType == kSenseMessageType
                             || record->messageType == kIncidentMessageType
                             || record->messageType == kAuthoritativeMessageType;
    if (specialized) {
        draw_client_message_detail(*record, snapshot, hasDetail ? &detail : nullptr);
    }
    if (!specialized) {
        draw_client_message_detail(*record, snapshot, hasDetail ? &detail : nullptr);
    }
    if (ImGui::TreeNodeEx("Technical details##client_packet", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Text("sequence: %llu", static_cast<unsigned long long>(record->sequence));
        ImGui::Text("activity: %llX.%llu",
                    static_cast<unsigned long long>(record->binding.sessionId),
                    static_cast<unsigned long long>(record->binding.createdRevision));
        ImGui::Text("source generation: %llu",
                    static_cast<unsigned long long>(record->sourceGeneration));
        ImGui::Text("peer mask: 0x%08X", static_cast<unsigned>(record->peerHeardMask));
        ImGui::Text("parser: %s", host::client_message_status_name(record->status));
        if (hasDetail && detail.hasSenseDecode) {
            ImGui::Text("Sense decode: %s", sense_update::decode_status_name(detail.sense.status));
            ImGui::Text("bits: %zu consumed, %zu remaining",
                        detail.sense.bitsConsumed,
                        detail.sense.bitsRemaining);
        }
        if (record->hasPayloadFingerprint) {
            ImGui::Text("run fingerprint: 0x%016llX",
                        static_cast<unsigned long long>(record->payloadFingerprint));
        }
        ImGui::TreePop();
    }
}

/** Draws retained metadata for owned client-to-Activity-Host messages. */
void draw_client_messages(const state::activity::SessionBinding* selected,
                          const host::DiagnosticsSnapshot& snapshot) noexcept {
    section::header("Incoming packets", nullptr);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Client activity packets, before the host reads them.");
    }
    ImGui::Checkbox("All activities", &g_showAllClientMessages);
    ImGui::SameLine();
    ImGui::Checkbox("Newest first", &g_clientMessagesNewestFirst);
    ImGui::SameLine();
    ImGui::Checkbox("Latest patch epoch only", &g_coalescePatchEpoch);
    draw_message_filter();
    std::size_t latestPatchEpoch = snapshot.clientMessageCount;
    for (std::size_t index = 0; index < snapshot.clientMessageCount; ++index) {
        const host::ClientMessageRecord& record = snapshot.clientMessages[index];
        if (client_message_visible(record, selected) && record.messageType == 52) {
            latestPatchEpoch = index;
        }
    }
    std::array<std::size_t, host::kClientMessageHistoryCapacity> visibleRows{};
    std::size_t visibleCount = 0;
    const auto add_visible = [&](std::size_t index) noexcept {
        const host::ClientMessageRecord& record = snapshot.clientMessages[index];
        if (!client_message_visible(record, selected)
            || (g_coalescePatchEpoch && record.messageType == 52 && index != latestPatchEpoch)) {
            return;
        }
        visibleRows[visibleCount++] = index;
    };
    if (g_clientMessagesNewestFirst) {
        for (std::size_t cursor = snapshot.clientMessageCount; cursor != 0; --cursor) {
            add_visible(cursor - 1);
        }
    } else {
        for (std::size_t index = 0; index < snapshot.clientMessageCount; ++index) {
            add_visible(index);
        }
    }
    const bool selectedVisible = std::any_of(
        visibleRows.begin(),
        visibleRows.begin() + static_cast<std::ptrdiff_t>(visibleCount),
        [&snapshot](std::size_t index) noexcept {
            return snapshot.clientMessages[index].sequence == g_selectedClientMessageSequence;
        });
    if (!selectedVisible) {
        g_selectedClientMessageSequence =
            visibleCount != 0 ? snapshot.clientMessages[visibleRows[0]].sequence : 0;
    }
    if (!ImGui::BeginTable("##activity_host_client_messages",
                           3,
                           kTableFlags | ImGuiTableFlags_Resizable,
                           table_layout::size(visibleCount, 12))) {
        return;
    }
    ImGui::TableSetupColumn("Packet", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visibleCount));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const host::ClientMessageRecord& record =
                snapshot.clientMessages[visibleRows[static_cast<std::size_t>(row)]];
            table_layout::next_row();
            ImGui::TableNextColumn();
            std::array<char, 160> label{};
            (void)std::snprintf(label.data(),
                                label.size(),
                                "%s##packet_%llu",
                                host::client_message_name(record.messageType),
                                static_cast<unsigned long long>(record.sequence));
            const bool selectedRow = record.sequence == g_selectedClientMessageSequence;
            if (table_layout::selectable(label.data(), selectedRow)) {
                g_selectedClientMessageSequence = record.sequence;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%u", record.payloadBytes);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(host::client_message_status_name(record.status));
        }
    }
    ImGui::EndTable();
    draw_selected_client_message(snapshot);
    if (snapshot.overwrittenClientMessages != 0) {
        ImGui::TextDisabled("%llu older packets overwritten",
                            static_cast<unsigned long long>(snapshot.overwrittenClientMessages));
    }
}

/** Draws recent events for the selected exact activity generation. */
void draw_events(const state::activity::SessionBinding& selected,
                 const host::DiagnosticsSnapshot& snapshot) noexcept {
    section::header("Host transitions", nullptr);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("What the host did with each packet.");
    }
    std::size_t visibleCount = 0;
    for (std::size_t index = 0; index < snapshot.eventCount; ++index) {
        visibleCount += same_binding(snapshot.events[index].binding, selected) ? 1U : 0U;
    }
    if (!ImGui::BeginTable(
            "##activity_host_events", 3, kTableFlags, table_layout::size(visibleCount))) {
        return;
    }
    ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Result");
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    table_layout::frozen_headers();
    for (std::size_t index = 0; index < snapshot.eventCount; ++index) {
        const host::Event& event = snapshot.events[index];
        if (!same_binding(event.binding, selected)) {
            continue;
        }
        table_layout::next_row();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(host::event_name(event.kind));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("sequence %llu; ingress %llu; source %llu; %u bytes",
                              static_cast<unsigned long long>(event.sequence),
                              static_cast<unsigned long long>(event.clientMessageSequence),
                              static_cast<unsigned long long>(event.sourceGeneration),
                              event.payloadBytes);
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(event_result(event));
        ImGui::TableNextColumn();
        draw_event_details(event);
    }
    ImGui::EndTable();
    if (snapshot.droppedIngress != 0 || snapshot.refusedControls != 0
        || snapshot.overwrittenEvents != 0) {
        ImGui::TextDisabled("Host diagnostics");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("dropped %llu; refused %llu; overwritten %llu",
                              static_cast<unsigned long long>(snapshot.droppedIngress),
                              static_cast<unsigned long long>(snapshot.refusedControls),
                              static_cast<unsigned long long>(snapshot.overwrittenEvents));
        }
    }
}

} // namespace

/** Draws packet content inside the Core-owned active tab. */
void draw(const host::InstanceSnapshot* instance,
          const host::DiagnosticsSnapshot& snapshot) noexcept {
    const auto* selected = instance != nullptr ? &instance->binding : nullptr;
    draw_client_messages(selected, snapshot);
    if (instance != nullptr) {
        ImGui::Spacing();
        draw_events(instance->binding, snapshot);
        ImGui::Spacing();
        incident_editor::draw(instance->binding, *instance, snapshot);
    } else
        ImGui::TextDisabled("No activity selected. Showing every packet.");
}
} // namespace sunrise::server::ui::activity_host::event_view
