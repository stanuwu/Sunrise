#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "../../content/packages/schema/generated/tag_classes_80804d.h"
#include "../../content/packages/schema/generated/tag_classes_80804e.h"
#include "../../content/packages/schema/generated/tag_classes_80804f.h"
#include "../../content/packages/schema/generated/tag_classes_808050.h"
#include "../../content/packages/schema/generated/tag_classes_808062.h"
#include "../../content/packages/schema/generated/tag_classes_80807d.h"
#include "../../content/packages/schema/generated/tag_classes_80807e.h"
#include "../../content/packages/schema/generated/tag_classes_80807f.h"
#include "../../content/packages/schema/generated/tag_classes_808090.h"
#include "../../content/packages/schema/generated/tag_classes_808094.h"
#include "../../content/packages/schema/generated/tag_classes_808095.h"
#include "../../content/packages/schema/generated/tag_classes_808099.h"

namespace sunrise::middleware::bap::activity_message::auth_schema_catalog {

namespace generated = middleware::content::packages::schema::generated;

/** Facts read from one complete ClientRef Auth table. */
struct Type final {
    std::string_view name{};
    std::uint32_t schema{};
    std::uint16_t structBytes{};
    std::uint16_t minimumBits{};
    std::uint16_t maximumBits{};
    std::uint16_t componentOffset{};
    std::uint8_t slotType{};
    bool hasContiguousMirror{};
    bool hasDynamicBody{};
    bool writable{};
};

/** Sentinel used when an applier does not keep one contiguous Auth mirror. */
inline constexpr std::uint16_t kNoComponentOffset = 0xFFFFU;

/**
 * The complete Auth type census. `writable` excludes only type 67, whose applier replaces the
 * process-wide faction-allegiance matrix rather than the selected slot.
 */
inline constexpr std::array<Type, 39> kTypes = {{
    {"squad_sensor", generated::C80807ec9::kClass, 196, 24, 1313, 384, 1, true, false, true},
    {"combatant_sensor", generated::C80807da1::kClass, 2392, 11, 8228, 384, 2, true, true, true},
    {"objective_sensor",
     generated::C80807f0c::kClass,
     28,
     2,
     225,
     kNoComponentOffset,
     3,
     false,
     false,
     true},
    {"object_sensor", generated::C8080992f::kClass, 368, 252, 525, 384, 4, true, true, true},
    {"sequence_sensor", generated::C80804f04::kClass, 1072, 7359, 7359, 384, 5, true, false, true},
    {"cinematic_sensor", generated::C80804f08::kClass, 224, 263, 1671, 384, 6, true, false, true},
    {"hud_sensor", generated::C80804f10::kClass, 464, 35, 3582, 384, 8, true, false, true},
    {"music_sensor", generated::C80804f58::kClass, 1048, 7223, 7223, 384, 11, true, false, true},
    {"player_spasaha_sensor",
     generated::C80804f30::kClass,
     760,
     128,
     4800,
     496,
     13,
     true,
     false,
     true},
    {"scoreboard_sensor", generated::C808099f9::kClass, 5904, 7, 47054, 384, 16, true, false, true},
    {"activity_lifetime_sensor",
     generated::C8080991a::kClass,
     1300,
     423,
     2265,
     384,
     17,
     true,
     false,
     true},
    {"activity_timer_sensor",
     generated::C80809919::kClass,
     64,
     386,
     386,
     384,
     18,
     true,
     false,
     true},
    {"player_navigation_sensor",
     generated::C80809527::kClass,
     7308,
     62,
     53150,
     384,
     19,
     true,
     false,
     true},
    {"damage_component_sensor",
     generated::C80809563::kClass,
     12,
     87,
     87,
     384,
     20,
     true,
     false,
     true},
    {"distance_sensor", generated::C80809502::kClass, 276, 98, 1868, 384, 21, true, false, true},
    {"device_sensor", generated::C80804f48::kClass, 24, 147, 147, 448, 23, true, false, true},
    {"channel_sensor", generated::C80804f40::kClass, 52, 3, 387, 384, 24, true, false, true},
    {"look_trigger_sensor", generated::C80804ed3::kClass, 16, 65, 129, 400, 25, true, false, true},
    {"hop_on_sensor", generated::C8080954b::kClass, 112, 186, 276, 384, 26, true, true, true},
    {"object_monitor_sensor", generated::C808094ea::kClass, 12, 87, 87, 384, 29, true, false, true},
    {"player_monitor_sensor", generated::C80809532::kClass, 12, 87, 87, 384, 30, true, false, true},
    {"player_trigger_sensor",
     generated::C80809524::kClass,
     24,
     129,
     129,
     392,
     31,
     true,
     false,
     true},
    {"toggle_sensor", generated::C8080955a::kClass, 12, 57, 57, 384, 32, true, false, true},
    {"object_filter_sensor", generated::C8080956a::kClass, 400, 4, 732, 384, 34, true, true, true},
    {"activity_hard_wipe_globals_sensor",
     generated::C808099bf::kClass,
     64,
     359,
     359,
     384,
     35,
     true,
     false,
     true},
    {"loot_sensor", generated::C80804efe::kClass, 16, 88, 88, 528, 36, true, false, true},
    {"map_generator_sensor",
     generated::C80805007::kClass,
     1460,
     1750,
     11350,
     384,
     37,
     true,
     false,
     true},
    {"task_sensor", generated::C80807d89::kClass, 4, 32, 32, 384, 38, true, false, true},
    {"passenger_sensor",
     generated::C80804ee5::kClass,
     12,
     86,
     86,
     kNoComponentOffset,
     39,
     false,
     false,
     true},
    {"target_sensor", generated::C8080955f::kClass, 8, 55, 55, 384, 40, true, false, true},
    {"new_user_experience_sensor",
     generated::C80804ee9::kClass,
     2120,
     12,
     9948,
     388,
     41,
     true,
     false,
     true},
    {"performance_sensor", generated::C80809586::kClass, 48, 2, 357, 384, 42, true, false, true},
    {"scene_sensor",
     generated::C8080626b::kClass,
     212,
     74,
     1538,
     kNoComponentOffset,
     43,
     false,
     false,
     true},
    {"dialog_sensor", generated::C80804f77::kClass, 4104, 19767, 27959, 384, 53, true, false, true},
    {"ghost_link_sensor", generated::C80804d3f::kClass, 12, 65, 65, 448, 65, true, false, true},
    {"team_side_sensor",
     generated::C808090aa::kClass,
     136,
     1057,
     1057,
     384,
     67,
     true,
     false,
     false},
    {"directive_sensor", generated::C80804f67::kClass, 768, 4802, 4802, 384, 68, true, false, true},
    {"encounter_engagement_sensor",
     generated::C808094f1::kClass,
     208,
     23,
     1496,
     384,
     70,
     true,
     false,
     true},
    {"public_event_sensor", generated::C80804f57::kClass, 24, 183, 183, 384, 71, true, false, true},
}};

/** Resolves only the exact slot-type and schema pair. */
[[nodiscard]] constexpr const Type* find(std::uint8_t slotType, std::uint32_t schema) noexcept {
    for (const Type& type : kTypes) {
        if (type.slotType == slotType && type.schema == schema) {
            return &type;
        }
    }
    return nullptr;
}

} // namespace sunrise::middleware::bap::activity_message::auth_schema_catalog
