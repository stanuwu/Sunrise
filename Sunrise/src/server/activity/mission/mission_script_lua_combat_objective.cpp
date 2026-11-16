#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../middleware/bap/activity_message/squad_objective_auth.h"
#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"
#include "mission_script_squad_sense.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

namespace objective = middleware::bap::activity_message::squad_objective;
namespace format = state::activity_sdk::format;
/** Returned task-group values compare by their two SDK identity fields. */
constexpr const char* kTaskGroupMetatable = "sunrise.mission.task_group";

// Table entries of the members this file implements.
constexpr const api::MemberSpec& kAssignObjective = api::slot_member("assign_combat_objective");
constexpr const api::MemberSpec& kTaskCounter = api::slot_member("task_counter");
constexpr const api::MemberSpec& kTaskCost =
    api::find_member(api::group_members(api::FieldGroupId::squadState), "task_cost");
constexpr const api::MemberSpec& kTaskGroupQuery =
    api::find_member(api::group_members(api::FieldGroupId::squadState), "task_group");
// The two identity fields of a generated mission.TaskGroup row.
constexpr const char* kSlotRowKey =
    table_field_key(kAssignObjective, "task_group", "slot_row", api::ValueKind::integer);
constexpr const char* kGroupIndexKey =
    table_field_key(kAssignObjective, "task_group", "group_index", api::ValueKind::integer);
// Reported costs and counters hold one entry per task group the assignment can name.
static_assert(kSquadObjectiveGroupCount == static_cast<std::size_t>(objective::kTaskGroupCount)
              && kObjectiveCapacity == static_cast<std::size_t>(objective::kTaskGroupCount));

/** @return True for an exact squad slot and an exact objective slot of the same registry. */
[[nodiscard]] bool combat_objective_pair(const SlotDefinition& squad,
                                         const SlotDefinition& target) noexcept {
    return squad.slotType == format::kSquadSlotType
           && squad.componentClass == format::kSquadComponentClass
           && squad.authSchema == objective::kSchema
           && target.slotType == format::kObjectiveSlotType
           && target.componentClass == format::kObjectiveComponentClass
           && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType3Schema
           && (squad.flags & format::kSlotSchemaJoinExact) != 0
           && (target.flags & format::kSlotSchemaJoinExact) != 0
           && squad.registryKey == target.registryKey;
}

/** Resolves one generated task-group reference through the pinned native SDK. */
[[nodiscard]] CombatObjectiveGroupDefinition task_group(lua_State* state, int table) {
    luaL_checktype(state, table, LUA_TTABLE);
    const lua_Integer slotRow = directive_integer(state, table, kSlotRowKey);
    const lua_Integer group = directive_integer(state, table, kGroupIndexKey);
    Impl* const impl = impl_from_state(state);
    CombatObjectiveGroupDefinition output{};
    if (slotRow < 0 || slotRow > (std::numeric_limits<std::uint32_t>::max)() || group < 0
        || group >= objective::kTaskGroupCount || impl == nullptr
        || impl->definitions.resolveCombatObjectiveGroup == nullptr
        || !impl->definitions.resolveCombatObjectiveGroup(impl->definitions.context,
                                                          static_cast<std::uint32_t>(slotRow),
                                                          static_cast<std::uint32_t>(group),
                                                          output)) {
        static_cast<void>(luaL_error(state, "task group does not belong to this SDK objective"));
    }
    return output;
}

/** Compares a returned task-group value with an extracted task-group constant. */
int task_group_equal(lua_State* state) {
    const auto left = task_group(state, 1);
    const auto right = task_group(state, 2);
    lua_pushboolean(state, left.slotRow == right.slotRow && left.groupIndex == right.groupIndex);
    return 1;
}

/** Keeps equality semantic while preventing scripts from replacing the native metatable. */
void set_task_group_metatable(lua_State* state) {
    if (luaL_newmetatable(state, kTaskGroupMetatable) != 0) {
        lua_pushcfunction(state, &task_group_equal);
        lua_setfield(state, -2, "__eq");
        lua_pushboolean(state, false);
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
}

/** Pushes one task-group value that compares equal to its generated mission.TaskGroup row. */
void push_task_group(lua_State* state, std::uint32_t slotRow, std::int32_t groupIndex) {
    lua_createtable(state, 0, 2);
    lua_pushinteger(state, slotRow);
    lua_setfield(state, -2, kSlotRowKey);
    lua_pushinteger(state, groupIndex);
    lua_setfield(state, -2, kGroupIndexKey);
    set_task_group_metatable(state);
}

} // namespace

/** Assigns a generated combat objective while native state owns evaluation revisions. */
int slot_assign_combat_objective(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kAssignObjective);
    using Kind = api::ValueKind;
    const auto reference = checked_argument<SlotHandle>(
        state, required_key(kAssignObjective, "objective", Kind::handle), kSlotMetatable);
    SlotDefinition squad{};
    SlotDefinition target{};
    if (!current_slot(state, *handle, squad) || !current_slot(state, reference, target)
        || !combat_objective_pair(squad, target)) {
        return luaL_error(state, "combat objective requires exact slots in the same registry");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::assignCombatObjective;
    intent.firstRow = squad.nativeRow;
    intent.secondRow = target.nativeRow;
    intent.entryIndex = objective::kNoTaskGroup;
    lua_getfield(state, 2, optional_key(kAssignObjective, "task_group", Kind::missionRow));
    if (!lua_isnil(state, -1)) {
        const auto group = task_group(state, -1);
        if (group.slotRow != target.nativeRow) {
            return luaL_error(state, "task group belongs to another objective");
        }
        intent.entryIndex = static_cast<std::int32_t>(group.groupIndex);
    }
    lua_pop(state, 1);
    intent.objectiveReconsider =
        optional_boolean_argument(state, boolean_option(kAssignObjective, "reconsider"));
    intent.objectiveRefreshAwareness = optional_boolean_argument(
        state, boolean_option(kAssignObjective, "refresh_player_awareness"));
    intent.squadTemperamentsSet = read_squad_temperaments(
        state, optional_key(kAssignObjective, "temperaments", Kind::list), squad.nativeRow, intent);
    const char* const reservedKey = optional_key(kAssignObjective, "reserved", Kind::boolean);
    // Left out, the reservation keeps its current value.
    const int reservedType = push_argument(state, reservedKey);
    if (reservedType != LUA_TNIL && reservedType != LUA_TBOOLEAN) {
        return luaL_error(state, "%s must be a boolean", reservedKey);
    }
    intent.objectivePreserveReservation = reservedType == LUA_TNIL;
    intent.objectiveReserved = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    if (!intent.objectiveReconsider && frame.event != nullptr
        && frame.event->kind == host::EventKind::squadState
        && frame.event->firstRegistryKey == squad.registryKey
        && frame.event->slotObjectTag == squad.objectTag
        && frame.event->firstSlotIndex == squad.slotIndex) {
        intent.expectedObjectiveRevision = frame.event->squadObjectiveRevision;
    }
    return queue_intent(state, frame, intent);
}

/** Reads one task counter of the objective as its Sense last reported it. */
int slot_task_counter(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kTaskCounter);
    SlotDefinition target{};
    if (!current_slot(state, *handle, target) || target.slotType != format::kObjectiveSlotType) {
        return luaL_error(state, "task_counter needs an SDK objective");
    }
    lua_getfield(state, 2, required_key(kTaskCounter, "group", api::ValueKind::missionRow));
    const auto group = task_group(state, -1);
    lua_pop(state, 1);
    if (group.slotRow != target.nativeRow) {
        return luaL_error(state, "task group belongs to another objective");
    }
    const lua_Integer task = checked_integer_argument(
        state, required_key(kTaskCounter, "task", api::ValueKind::integer));
    if (task < 0 || task >= static_cast<lua_Integer>(kObjectiveTaskCapacity)
        || task >= static_cast<lua_Integer>(group.taskCount)) {
        return luaL_error(state, "task is outside an objective group's tasks");
    }
    const Impl* const impl = impl_from_state(state);
    const auto levels = std::span(impl->objectiveLevels).first(impl->objectiveLevelCount);
    const auto level =
        std::find_if(levels.begin(), levels.end(), [&target](const ObjectiveLevelRow& row) {
            return row.registryKey == target.registryKey && row.objectTag == target.objectTag
                   && row.slotIndex == target.slotIndex;
        });
    const auto taskIndex = static_cast<std::size_t>(task);
    if (level == levels.end()
        || (level->known[group.groupIndex] & (std::uint32_t{1} << taskIndex)) == 0) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, level->counters[group.groupIndex][taskIndex]);
    }
    return 1;
}

/** Returns a reachable cost and whether this report supplies a qualified value for the group. */
int event_task_cost(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    refuse_unknown_arguments(state, kTaskCost);
    lua_getfield(state, 2, required_key(kTaskCost, "group", api::ValueKind::missionRow));
    const auto group = task_group(state, -1);
    lua_pop(state, 1);
    const bool qualified = event.kind == host::EventKind::squadState
                           && event.squadObjectiveCostQualified
                           && group.registryKey == event.squadObjectiveRegistryKey
                           && group.slotIndex == event.squadObjectiveSlotIndex
                           && (event.squadObjectiveCostMask & (1U << group.groupIndex)) != 0;
    const float cost = event.squadObjectiveCosts[group.groupIndex];
    const bool known =
        qualified && std::isfinite(cost) && cost >= 0 && cost <= kMaximumObjectiveCost;
    if (known && cost < kMaximumObjectiveCost) {
        lua_pushnumber(state, cost);
    } else {
        lua_pushnil(state);
    }
    lua_pushboolean(state, known);
    return 2;
}

/** Returns the selected authored group and whether the squad is assigned to this objective. */
int event_task_group(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    refuse_unknown_arguments(state, kTaskGroupQuery);
    const auto handle = checked_argument<SlotHandle>(
        state, required_key(kTaskGroupQuery, "objective", api::ValueKind::handle), kSlotMetatable);
    SlotDefinition target{};
    if (!current_slot(state, handle, target) || target.slotType != format::kObjectiveSlotType) {
        return luaL_error(state, "task group needs an SDK objective");
    }
    const bool assigned = event.kind == host::EventKind::squadState
                          && event.squadObjectiveRegistryKey == target.registryKey
                          && event.squadObjectiveSlotIndex == target.slotIndex;
    if (assigned && event.squadObjectiveTaskGroup >= 0) {
        push_task_group(state, target.nativeRow, event.squadObjectiveTaskGroup);
    } else {
        lua_pushnil(state);
    }
    lua_pushboolean(state, assigned);
    return 2;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
