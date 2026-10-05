#include <algorithm>
#include <limits>

#include "../../middleware/bap/activity_message/damage_monitor_auth.h"
#include "../../middleware/bap/activity_message/darkness_zone_auth.h"
#include "../../middleware/bap/activity_message/ghost_link_auth.h"
#include "../../middleware/bap/activity_message/interactable_object_auth.h"
#include "../../middleware/bap/activity_message/map_generator_auth.h"
#include "../../middleware/bap/activity_message/mission_effect_auth.h"
#include "../../middleware/bap/activity_message/music_section_auth.h"
#include "../../middleware/bap/activity_message/scene_events_auth.h"
#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../middleware/bap/activity_message/squad_objective_auth.h"
#include "../../middleware/content/packages/tables/region_reader.h"
#include "../../state/activity/runtime.h"
#include "activity_sdk_device_internal.h"

namespace sunrise::server::activity::activity_sdk_devices::detail {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace format = state::activity_sdk::format;
namespace layouts = state::build_data::scenarios;
namespace sdk = state::activity_sdk;
namespace tables = middleware::content::packages::tables;

static_assert(format::kDeviceSlotType == auth::kType23SlotType);
static_assert(format::kDeviceAuthSchema == auth::kType23Schema);
static_assert(format::kObjectSlotType == auth::kType4SlotType);
static_assert(format::kObjectAuthSchema == auth::kType4Schema);

namespace {

/** @return True when both complete groups have the same key and compressed wire layout. */
[[nodiscard]] bool same_roster_layout(const layouts::RosterGroup& left,
                                      const layouts::RosterGroup& right) noexcept {
    if (!layouts::valid_roster_group(left) || !layouts::valid_roster_group(right)
        || left.registryKey != right.registryKey || left.slotCount != right.slotCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.slotCount; ++index) {
        if (left.slotTypes[index] != right.slotTypes[index]
            || left.slotFlags[index] != right.slotFlags[index]
            || left.slotIndices[index] != right.slotIndices[index]) {
            return false;
        }
    }
    return true;
}

/** Finds the selected Auth slot in one materialized group's compressed order. */
[[nodiscard]] bool selected_slot_offset(const layouts::RosterGroup& group,
                                        const format::Slot& slot,
                                        std::uint16_t& output) noexcept {
    output = 0;
    if (!layouts::valid_roster_group(group)
        || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    std::size_t matches = 0;
    for (std::size_t index = 0; index < group.slotCount; ++index) {
        if (group.slotIndices[index] != slot.slotIndex || group.slotTypes[index] != slot.slotType
            || (group.slotFlags[index] & layouts::kSlotAuthFlag) == 0) {
            continue;
        }
        output = static_cast<std::uint16_t>(index);
        ++matches;
    }
    return matches == 1;
}

/** Resolves a materialized occurrence group to at most one canonical roster row. */
[[nodiscard]] Status canonical_target(const sdk::BoundView& view,
                                      const format::Scenario& scenario,
                                      const format::Slot& slot,
                                      const layouts::RosterGroup& generatedGroup,
                                      std::int32_t effectiveRegion,
                                      host::ScriptableTarget& output,
                                      bool& found) noexcept {
    output = {};
    found = false;
    if (view.binding.destination.packageNameLength == 0
        || view.binding.destination.packageNameLength
               > view.binding.destination.packageName.size()) {
        return Status::invalidView;
    }
    const std::string_view destination(
        reinterpret_cast<const char*>(view.binding.destination.packageName.data()),
        view.binding.destination.packageNameLength);
    layouts::Definition layout{};
    if (!state::build_data::find_scenario_layout(destination, layout) || layout.tag != scenario.tag
        || layout.rosterGroupCount > layout.rosterGroups.size()
        || layout.bubbleGroupCount > layout.bubbleGroups.size()) {
        return Status::targetUnavailable;
    }
    const std::uint32_t activeBubble =
        static_cast<std::uint32_t>(effectiveRegion) / tables::kSliceSetIndexFactor;
    std::size_t activeMatches = 0;
    std::uint16_t selectedTable = 0;
    layouts::RosterGroup selectedGroup{};
    const auto consider = [&](std::uint16_t tableIndex, bool active) noexcept {
        layouts::RosterGroup candidate{};
        if (!state::build_data::find_roster_group(tableIndex, candidate)) {
            return false;
        }
        if (!same_roster_layout(generatedGroup, candidate)) {
            return true;
        }
        if (active) {
            if (activeMatches == 0) {
                activeMatches = 1;
                selectedTable = tableIndex;
                selectedGroup = candidate;
            } else if (tableIndex != selectedTable) {
                // The same canonical table may be published in both the always-active and bubble
                // lanes. That is one target; only a distinct active table is ambiguous.
                activeMatches = 2;
            }
        }
        return true;
    };
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        if (!consider(layout.rosterGroups[index], true)) {
            return Status::targetUnavailable;
        }
    }
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        const bool active =
            activeBubble < layouts::kBubbleCapacity
            && (layout.bubbleGroupMasks[index] & (std::uint64_t{1} << activeBubble)) != 0;
        if (!consider(layout.bubbleGroups[index], active)) {
            return Status::targetUnavailable;
        }
    }
    if (activeMatches > 1) {
        return Status::ambiguousTarget;
    }
    if (activeMatches == 0) {
        return Status::ready;
    }
    std::uint16_t slotOffset = 0;
    if (!selected_slot_offset(selectedGroup, slot, slotOffset)) {
        return Status::invalidSlot;
    }
    output.objectTag = selectedGroup.objectTag;
    output.registryKey = selectedGroup.registryKey;
    output.authSchema = slot.authSchema;
    output.rosterGroupIndex = selectedTable;
    output.rosterSlotOffset = slotOffset;
    output.slotIndex = static_cast<std::uint16_t>(slot.slotIndex);
    output.slotType = static_cast<std::uint8_t>(slot.slotType);
    found = true;
    return Status::ready;
}

/** Finishes the exact canonical or state-local route resolved from the bound SDK. */
[[nodiscard]] Status finish_prepared(const sdk::BoundView& view,
                                     const server::bap::ActivityLinkView& link,
                                     PreparedDevice& output) noexcept {
    output.activityClientGeneration = link.activityClientGeneration;
    output.scenarioRow = view.scenarioRow;
    output.effectiveRegion = link.effectiveRegion;
    return Status::ready;
}
} // namespace

/** Maps the SDK binding validator to this API's stable refusal surface. */
[[nodiscard]] Status binding_status(const sdk::BoundView& view,
                                    server::bap::ActivityLinkView& link) noexcept {
    if (view.catalog == nullptr || sdk::bound_activity(view) == nullptr
        || sdk::bound_scenario(view) == nullptr) {
        return Status::invalidView;
    }
    if (!state::activity::binding_matches(view.binding)) {
        return Status::staleBinding;
    }
    (void)server::bap::activity_link_view(view.binding, view.activityClientGeneration, link);
    switch (
        sdk::revalidate(view, view.binding, link.matchingLinks, link.activityClientGeneration)) {
    case sdk::Status::ready:
        return link.effectiveRegion >= 0 ? Status::ready : Status::noActivityLink;
    case sdk::Status::missingClient:
    case sdk::Status::ambiguousClient:
        return Status::noActivityLink;
    case sdk::Status::staleSession:
        return Status::staleBinding;
    case sdk::Status::staleActivityClient:
        return Status::staleActivityClient;
    case sdk::Status::notReady:
    case sdk::Status::missing:
    case sdk::Status::wrongSdkBuild:
    case sdk::Status::catalogInvalid:
    case sdk::Status::wrongActivity:
    case sdk::Status::activityJoinNotExact:
    case sdk::Status::missingScenarioLink:
        return Status::invalidView;
    }
    return Status::invalidView;
}

/** Resolves one SDK slot through a published canonical group or its exact live occurrence. */
[[nodiscard]] Status
prepare_slot(const sdk::BoundView& view, std::uint32_t slotRow, PreparedDevice& output) noexcept {
    output = {};
    server::bap::ActivityLinkView link{};
    const Status live = binding_status(view, link);
    if (live != Status::ready) {
        return live;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto slots = catalog.slots();
    const auto objects = catalog.objects();
    const auto occurrences = catalog.occurrences();
    const auto states = catalog.states();
    const auto bubbles = catalog.bubbles();
    if (scenario == nullptr || slotRow >= slots.size()) {
        return Status::invalidSlot;
    }
    const format::Slot& slot = slots[slotRow];
    if (slot.objectIndex >= objects.size()
        || slot.slotIndex > middleware::bap::activity_message::sensor_auth_update::kMaximumSlotIndex
        || slot.slotType > middleware::bap::activity_message::sensor_auth_update::kMaximumSlotType
        || slot.authSchema == 0 || slot.authSchema == format::kAbsentIndex) {
        return Status::invalidSlot;
    }
    if ((slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return Status::schemaJoinNotExact;
    }

    const format::Object& object = objects[slot.objectIndex];
    if (object.objectTag == 0 || object.objectKey == 0
        || !sdk::materialize_roster_group(catalog, object, output.generatedRosterGroup)) {
        return Status::invalidSlot;
    }
    std::uint16_t slotOffset = 0;
    if (!selected_slot_offset(output.generatedRosterGroup, slot, slotOffset)) {
        return Status::invalidSlot;
    }

    bool canonical = false;
    const Status canonicalStatus = canonical_target(view,
                                                    *scenario,
                                                    slot,
                                                    output.generatedRosterGroup,
                                                    link.effectiveRegion,
                                                    output.target,
                                                    canonical);
    if (canonicalStatus != Status::ready) {
        output = {};
        return canonicalStatus;
    }
    if (canonical) {
        return finish_prepared(view, link, output);
    }

    const format::Occurrence* selected = nullptr;
    std::uint32_t selectedState = format::kAbsentIndex;
    for (const format::Occurrence& occurrence : sdk::scenario_occurrences(catalog, *scenario)) {
        if (occurrence.scenarioIndex != view.scenarioRow || &occurrence < occurrences.data()
            || &occurrence >= occurrences.data() + occurrences.size()) {
            return Status::invalidView;
        }
        if (occurrence.objectIndex != slot.objectIndex) {
            continue;
        }
        if (occurrence.stateIndex >= states.size() || occurrence.bubbleIndex >= bubbles.size()) {
            return Status::invalidView;
        }
        const format::State& candidateState = states[occurrence.stateIndex];
        const format::Bubble& candidateBubble = bubbles[occurrence.bubbleIndex];
        if (candidateState.scenarioIndex != view.scenarioRow
            || candidateBubble.scenarioIndex != view.scenarioRow
            || candidateState.bubbleIndex != occurrence.bubbleIndex
            || candidateState.stateOrdinal >= tables::kSliceSetIndexFactor
            || candidateBubble.bubbleOrdinal >= layouts::kBubbleCapacity) {
            return Status::invalidView;
        }
        const std::uint64_t candidateRegion =
            static_cast<std::uint64_t>(candidateState.sliceSetIndex) + candidateState.stateOrdinal;
        if (candidateRegion != static_cast<std::uint64_t>(link.effectiveRegion)) {
            continue;
        }
        if (selected == nullptr) {
            selected = &occurrence;
            selectedState = occurrence.stateIndex;
        } else if (occurrence.stateIndex != selectedState) {
            return Status::ambiguousTarget;
        }
    }
    if (selected == nullptr) {
        return Status::targetUnavailable;
    }
    if (selected->stateIndex >= states.size() || selected->bubbleIndex >= bubbles.size()
        || selected->objectIndex >= objects.size()) {
        return Status::invalidView;
    }
    const format::State& state = states[selected->stateIndex];
    const format::Bubble& bubble = bubbles[selected->bubbleIndex];
    if (state.scenarioIndex != view.scenarioRow || bubble.scenarioIndex != view.scenarioRow
        || state.bubbleIndex != selected->bubbleIndex || object.objectTag == 0
        || object.objectKey == 0 || state.stateOrdinal >= tables::kSliceSetIndexFactor
        || bubble.bubbleOrdinal >= layouts::kBubbleCapacity) {
        return Status::invalidView;
    }
    output.target.objectTag = output.generatedRosterGroup.objectTag;
    output.target.registryKey = output.generatedRosterGroup.registryKey;
    output.target.authSchema = slot.authSchema;
    output.target.rosterGroupIndex = host::kGeneratedRosterGroupIndex;
    output.target.rosterSlotOffset = slotOffset;
    output.target.slotIndex = static_cast<std::uint16_t>(slot.slotIndex);
    output.target.sdkObjectIndex = selected->objectIndex;
    output.target.stateLocalRegion = link.effectiveRegion;
    output.target.slotType = static_cast<std::uint8_t>(slot.slotType);
    output.target.stateLocalRoster = true;
    output.stateRow = selected->stateIndex;
    return finish_prepared(view, link, output);
}

/** Resolves the legacy type-23 device facade through the shared typed SDK Auth slot route. */
[[nodiscard]] Status prepare(const sdk::BoundView& view,
                             std::uint32_t slotRow,
                             auth::Type23Channel channel,
                             float value,
                             PreparedDevice& output) noexcept {
    (void)value;
    const auto channelIndex = static_cast<std::size_t>(channel);
    if (channelIndex >= auth::kType23ChannelCount) {
        return Status::invalidChannel;
    }
    if (view.catalog == nullptr || slotRow >= view.catalog->slots().size()) {
        return Status::invalidSlot;
    }
    const format::Slot& slot = view.catalog->slots()[slotRow];
    if (slot.slotType != format::kDeviceSlotType
        || slot.componentClass != format::kDeviceComponentClass
        || slot.senseSchema != format::kDeviceSenseSchema
        || slot.authSchema != format::kDeviceAuthSchema) {
        return Status::invalidSlot;
    }
    return prepare_slot(view, slotRow, output);
}

/**
 * Resolves one exact type-31 trigger slot. Type 31 carries no caller value, so unlike the device
 * path there is no channel or range to check first.
 * @param view Pinned SDK view whose binding is revalidated.
 * @param slotRow Catalog row the pulse targets.
 * @param output Cleared, then receives the resolved target.
 * @return ready only for an exact type-31 slot on a live owned binding.
 */
[[nodiscard]] Status prepare_trigger(const sdk::BoundView& view,
                                     std::uint32_t slotRow,
                                     PreparedDevice& output) noexcept {
    if (view.catalog == nullptr || slotRow >= view.catalog->slots().size()) {
        return Status::invalidSlot;
    }
    const format::Slot& slot = view.catalog->slots()[slotRow];
    if (slot.slotType != auth::kType31SlotType || slot.authSchema != auth::kType31Schema) {
        return Status::invalidSlot;
    }
    return prepare_slot(view, slotRow, output);
}

/** Resolves one package-owned object entry without accepting a caller transform. */
[[nodiscard]] Status prepare_object(const sdk::BoundView& view,
                                    std::uint32_t slotRow,
                                    std::int32_t entryIndex,
                                    PreparedDevice& output) noexcept {
    if (view.catalog == nullptr || slotRow >= view.catalog->slots().size() || entryIndex < 0) {
        return Status::invalidSlot;
    }
    const format::Slot& slot = view.catalog->slots()[slotRow];
    if (slot.slotType != format::kObjectSlotType
        || slot.componentClass != format::kObjectComponentClass
        || slot.senseSchema != format::kObjectSenseSchema
        || slot.authSchema != format::kObjectAuthSchema) {
        return Status::invalidSlot;
    }
    return prepare_slot(view, slotRow, output);
}

/** Resolves one exact actor channel bridge. */
[[nodiscard]] Status prepare_combatant(const sdk::BoundView& view,
                                       std::uint32_t slotRow,
                                       PreparedDevice& output) noexcept {
    if (view.catalog == nullptr || slotRow >= view.catalog->slots().size()) {
        return Status::invalidSlot;
    }
    const format::Slot& slot = view.catalog->slots()[slotRow];
    if (slot.slotType != auth::kType2SlotType || slot.componentClass != auth::kType2ComponentClass
        || slot.senseSchema != auth::kType2SenseSchema || slot.authSchema != auth::kType2Schema) {
        return Status::invalidSlot;
    }
    return prepare_slot(view, slotRow, output);
}

/** Verifies exact SDK identity and schema-decodes one retained Auth body. */
[[nodiscard]] Status validate_auth(const sdk::BoundView& view,
                                   std::uint32_t slotRow,
                                   std::uint32_t objectTag,
                                   std::uint32_t registryKey,
                                   std::uint32_t authSchema,
                                   std::uint16_t slotIndex,
                                   std::uint8_t slotType,
                                   std::span<const std::byte> body,
                                   std::uint16_t bitCount,
                                   std::span<const std::byte> sdkBuildSha256) noexcept {
    if (view.catalog == nullptr || sdkBuildSha256.size() != 32U
        || view.catalog->sdk_build_sha256().size() != sdkBuildSha256.size()
        || !std::equal(sdkBuildSha256.begin(),
                       sdkBuildSha256.end(),
                       view.catalog->sdk_build_sha256().begin())) {
        return Status::wrongSdkBuild;
    }
    const auto slots = view.catalog->slots();
    const auto objects = view.catalog->objects();
    if (slotRow >= slots.size()) {
        return Status::invalidSlot;
    }
    const format::Slot& slot = slots[slotRow];
    if (slot.objectIndex >= objects.size() || objectTag == 0 || registryKey == 0 || authSchema == 0
        || slotType > middleware::bap::activity_message::sensor_auth_update::kMaximumSlotType
        || slotIndex > middleware::bap::activity_message::sensor_auth_update::kMaximumSlotIndex
        || objects[slot.objectIndex].objectTag != objectTag
        || objects[slot.objectIndex].objectKey != registryKey || slot.authSchema != authSchema
        || slot.slotIndex != slotIndex || slot.slotType != slotType) {
        return Status::invalidSlot;
    }
    if ((slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return Status::schemaJoinNotExact;
    }
    if (body.empty()
        || body.size()
               > middleware::bap::activity_message::sensor_auth_update::kAuthOverrideByteCapacity
        || bitCount > body.size() * 8U || bitCount + 7U < body.size() * 8U) {
        return Status::invalidBody;
    }
    const std::size_t trailingBits = bitCount % 8U;
    if (trailingBits != 0) {
        const std::uint8_t paddingMask =
            static_cast<std::uint8_t>((std::uint16_t{1} << (8U - trailingBits)) - 1U);
        if ((std::to_integer<std::uint8_t>(body.back()) & paddingMask) != 0) {
            return Status::invalidBody;
        }
    }
    // An occupancy Auth body is exactly 87 bits, which the wire pads to 11 bytes.
    constexpr std::size_t kOccupancyAuthBits = 87;
    constexpr std::size_t kOccupancyAuthBytes = 11;
    // Bodies this tree encodes are admitted by slot identity. Their shape belongs to the encoder.
    namespace message = middleware::bap::activity_message;
    const auto typed = [&](std::uint32_t type, std::uint32_t componentClass, std::uint32_t schema) {
        return slotType == type && slot.componentClass == componentClass && authSchema == schema;
    };
    const bool typedSdkBody =
        typed(auth::kType34SlotType, auth::kType34ComponentClass, auth::kType34Schema)
        || typed(message::mission_effect::kSlotType,
                 message::mission_effect::kComponentClass,
                 message::mission_effect::kSchema)
        || typed(message::music_section::kSlotType,
                 message::music_section::kComponentClass,
                 message::music_section::kSchema)
        || typed(message::scene_events::kSlotType,
                 message::scene_events::kComponentClass,
                 message::scene_events::kSchema)
        || typed(message::damage_monitor::kSlotType,
                 message::damage_monitor::kComponentClass,
                 message::damage_monitor::kAuthSchema)
        || typed(message::darkness_zone::kSlotType,
                 message::darkness_zone::kComponentClass,
                 message::darkness_zone::kSchema)
        || typed(format::kObjectSlotType,
                 format::kObjectComponentClass,
                 message::interactable_object::kSchema)
        || typed(message::ghost_link::kSlotType,
                 message::ghost_link::kComponentClass,
                 message::ghost_link::kAuthSchema)
        || typed(
            format::kSquadSlotType, format::kSquadComponentClass, message::squad_objective::kSchema)
        || typed(message::map_generator_auth::kSlotType,
                 message::map_generator_auth::kComponentClass,
                 message::map_generator_auth::kSchema)
        || typed(auth::kType2SlotType, auth::kType2ComponentClass, auth::kType2Schema);
    const bool occupancy = slotType == format::kOccupancySlotType
                           && authSchema == format::kOccupancyAuthSchema
                           && bitCount == kOccupancyAuthBits && body.size() == kOccupancyAuthBytes;
    const bool directive =
        slotType == middleware::bap::activity_message::scriptable_auth::kType68SlotType
        && authSchema == middleware::bap::activity_message::scriptable_auth::kType68Schema
        && middleware::bap::activity_message::scriptable_auth::validate_type68_body(body, bitCount);
    const bool engagement =
        slotType == middleware::bap::activity_message::scriptable_auth::kType70SlotType
        && authSchema == middleware::bap::activity_message::scriptable_auth::kType70Schema
        && middleware::bap::activity_message::scriptable_auth::validate_type70_body(body, bitCount);
    const bool publicEvent =
        slotType == middleware::bap::activity_message::scriptable_auth::kType71SlotType
        && authSchema == middleware::bap::activity_message::scriptable_auth::kType71Schema
        && middleware::bap::activity_message::scriptable_auth::validate_type71_body(body, bitCount);
    const bool performance =
        slotType == middleware::bap::activity_message::scriptable_auth::kType42SlotType
        && authSchema == middleware::bap::activity_message::scriptable_auth::kType42Schema
        && middleware::bap::activity_message::scriptable_auth::validate_type42_body(body, bitCount);
    const bool combatant =
        slotType == middleware::bap::activity_message::scriptable_auth::kType2SlotType
        && authSchema == middleware::bap::activity_message::scriptable_auth::kType2Schema
        && middleware::bap::activity_message::scriptable_auth::validate_type2_body(body, bitCount);
    if (!typedSdkBody && !occupancy && !directive && !engagement && !publicEvent && !performance
        && !combatant) {
        return Status::invalidBody;
    }
    return Status::ready;
}

} // namespace sunrise::server::activity::activity_sdk_devices::detail
