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
constexpr const api::MemberSpec& kSelectTask = api::slot_member("select_combat_task");
constexpr const char* kAllowedKey = optional_key(kSelectTask, "allowed", api::ValueKind::list);
constexpr const char* kPreferredKey = optional_key(kSelectTask, "preferred", api::ValueKind::list);
constexpr const api::MemberSpec& kTaskCost =
    api::find_member(api::group_members(api::FieldGroupId::squadState), "task_cost");
constexpr const api::MemberSpec& kTaskGroupQuery =
    api::find_member(api::group_members(api::FieldGroupId::squadState), "task_group");
// The two identity fields of a generated mission.TaskGroup row.
constexpr const char* kSlotRowKey =
    table_field_key(kAssignObjective, "task_group", "slot_row", api::ValueKind::integer);
constexpr const char* kGroupIndexKey =
    table_field_key(kAssignObjective, "task_group", "group_index", api::ValueKind::integer);
// Retained costs and counters hold one entry per task group the assignment can name.
static_assert(kSquadObjectiveGroupCount == static_cast<std::size_t>(objective::kTaskGroupCount)
              && kObjectiveCapacity == static_cast<std::size_t>(objective::kTaskGroupCount));

/** @return True for an exact squad slot and an exact objective slot of the same registry. */
[[nodiscard]] bool combat_objective_pair(const SlotDefinition& squad,
                                         const SlotDefinition& target) noexcept {
    return squad.slotType == format::kSquadSlotType
           && squad.componentClass == format::kSquadComponentClass
           && squad.authSchema == objective::kSchema && target.slotType == format::kObjectiveSlotType
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

/**
 * Reads an optional list of the objective's task groups.
 * @return One bit per listed group, or `absent` when the list is left out.
 */
[[nodiscard]] std::uint32_t task_group_mask(lua_State* state,
                                            const char* key,
                                            std::uint32_t objectiveRow,
                                            std::uint32_t absent) {
    if (push_argument(state, key) == LUA_TNIL) {
        lua_pop(state, 1);
        return absent;
    }
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    if (count > static_cast<std::size_t>(objective::kTaskGroupCount)) {
        static_cast<void>(luaL_error(state, "%s names more groups than an objective holds", key));
    }
    std::uint32_t mask = 0;
    for (std::size_t index = 1; index <= count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index));
        const CombatObjectiveGroupDefinition group = task_group(state, -1);
        lua_pop(state, 1);
        if (group.slotRow != objectiveRow) {
            static_cast<void>(luaL_error(state, "%s names another objective's task group", key));
        }
        mask |= std::uint32_t{1} << group.groupIndex;
    }
    lua_pop(state, 1);
    return mask;
}

/**
 * @return The squad's whole membership in this attempt: its placed count, or more when more were
 * seen in the placed generation. Zero while it has no placement.
 */
[[nodiscard]] std::int32_t squad_membership(const Impl& impl, std::uint32_t squadRow) noexcept {
    const auto found = std::find_if(
        impl.squadPopulations.begin(), impl.squadPopulations.end(), [&](const auto& population) {
            return population.squadRow == squadRow
                   && population.attemptGeneration == impl.attempt.generation;
        });
    if (found == impl.squadPopulations.end()) {
        return 0;
    }
    if (!found->generationKnown) {
        return found->expectedAlive;
    }
    return (std::max)(
        {found->expectedAlive, found->maximumObservedAlive, found->maximumObservedCreated});
}

/** @return True while an assignment of the squad waits in this callback or in the outbox. */
[[nodiscard]] bool assignment_pending(lua_State* state, const Impl& impl, std::uint32_t squadRow) {
    const auto assigns = [squadRow](const Intent& intent) noexcept {
        return intent.kind == IntentKind::assignCombatObjective && intent.firstRow == squadRow;
    };
    const auto& candidate = active_frame(state).candidate.intents;
    const auto outbox = std::span(impl.outbox).subspan(impl.outboxRead);
    return std::any_of(candidate.begin(), candidate.end(), assigns)
           || std::any_of(outbox.begin(), outbox.end(), assigns);
}

/**
 * @return True when one authored task of the group admits a squad of `members`. A task stops
 * admitting once its reported counter reaches its limit. The squads already holding a task are
 * not reported, so a squad limit refuses only a task that admits no squad.
 * @param counters The objective's retained counters; null before it reported any.
 */
[[nodiscard]] bool group_admits(const CombatObjectiveGroupDefinition& group,
                                std::int32_t members,
                                const ObjectiveLevelRow* counters) noexcept {
    for (std::uint32_t index = 0; index < group.taskCount; ++index) {
        const format::CombatTaskBounds& task = group.tasks[index];
        // A bound below -1 has no known meaning, so its task is never offered.
        if (task.minimumMembers < -1 || task.maximumMembers < -1 || task.maximumSquads < -1
            || task.counterLimit < -1) {
            continue;
        }
        const bool exhausted =
            task.counterLimit >= 0 && counters != nullptr
            && (counters->known[group.groupIndex] & (std::uint32_t{1} << index)) != 0
            && counters->counters[group.groupIndex][index] >= task.counterLimit;
        if (members >= task.minimumMembers
            && (task.maximumMembers < 0 || members <= task.maximumMembers)
            && task.maximumSquads != 0 && !exhausted) {
            return true;
        }
    }
    return false;
}

/**
 * Picks the policy group of least reachable cost whose tasks admit the squad.
 * The squad's assigned group wins a tie, so an equal cost never reassigns it.
 * @return The group index, or objective::kNoTaskGroup when no policy group qualifies.
 */
[[nodiscard]] std::int32_t cheapest_group(const Impl& impl,
                                          std::uint32_t objectiveRow,
                                          const SquadObjectiveOffer& offer,
                                          const ObjectiveLevelRow* counters,
                                          std::int32_t members,
                                          std::uint32_t policy) noexcept {
    std::int32_t selected = objective::kNoTaskGroup;
    float least = kMaximumObjectiveCost;
    for (std::int32_t index = 0; index < objective::kTaskGroupCount; ++index) {
        const std::uint32_t bit = std::uint32_t{1} << index;
        const float cost = offer.costs.values[static_cast<std::size_t>(index)];
        // The saturated maximum is the client's unreachable cost.
        if ((policy & offer.costs.known & bit) == 0 || cost >= kMaximumObjectiveCost
            || cost > least || (cost == least && index != offer.taskGroup)) {
            continue;
        }
        CombatObjectiveGroupDefinition group{};
        if (impl.definitions.resolveCombatObjectiveGroup(
                impl.definitions.context, objectiveRow, static_cast<std::uint32_t>(index), group)
            && group_admits(group, members, counters)) {
            selected = index;
            least = cost;
        }
    }
    return selected;
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

/**
 * Assigns the squad its reachable allowed group of least cost whose tasks admit the whole squad.
 * The costs are retained, so any callback can choose; preferred groups are tried first.
 */
int slot_select_combat_task(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kSelectTask);
    const auto reference = checked_argument<SlotHandle>(
        state, required_key(kSelectTask, "objective", api::ValueKind::handle), kSlotMetatable);
    SlotDefinition squad{};
    SlotDefinition target{};
    if (!current_slot(state, *handle, squad) || !current_slot(state, reference, target)
        || !combat_objective_pair(squad, target)) {
        return luaL_error(state, "task selection requires exact slots in the same registry");
    }
    const Impl* const impl = impl_from_state(state);
    // Groups are numbered densely from zero, so the first unresolved index ends them.
    std::uint32_t authored = 0;
    for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(objective::kTaskGroupCount);
         ++index) {
        CombatObjectiveGroupDefinition group{};
        if (impl->definitions.resolveCombatObjectiveGroup == nullptr
            || !impl->definitions.resolveCombatObjectiveGroup(
                impl->definitions.context, target.nativeRow, index, group)) {
            break;
        }
        authored |= std::uint32_t{1} << index;
    }
    const std::uint32_t allowed = task_group_mask(state, kAllowedKey, target.nativeRow, authored);
    const std::uint32_t preferred =
        task_group_mask(state, kPreferredKey, target.nativeRow, 0) & allowed;
    const auto offer = std::find_if(
        impl->objectiveOffers.begin(), impl->objectiveOffers.end(), [&](const auto& retained) {
            return retained.registryKey == squad.registryKey
                   && retained.objectTag == squad.objectTag
                   && retained.slotIndex == squad.slotIndex
                   && retained.objectiveRegistryKey == target.registryKey
                   && retained.objectiveSlotIndex == target.slotIndex;
        });
    const std::int32_t members = squad_membership(*impl, squad.nativeRow);
    if (authored == 0 || offer == impl->objectiveOffers.end() || members <= 0
        || assignment_pending(state, *impl, squad.nativeRow)) {
        lua_pushnil(state);
        lua_pushboolean(state, false);
        return 2;
    }
    const auto levels = std::span(impl->objectiveLevels).first(impl->objectiveLevelCount);
    const auto level =
        std::find_if(levels.begin(), levels.end(), [&target](const ObjectiveLevelRow& row) {
            return row.registryKey == target.registryKey && row.objectTag == target.objectTag
                   && row.slotIndex == target.slotIndex;
        });
    const ObjectiveLevelRow* const counters = level != levels.end() ? &*level : nullptr;
    std::int32_t selected = objective::kNoTaskGroup;
    if (preferred != 0) {
        selected = cheapest_group(*impl, target.nativeRow, *offer, counters, members, preferred);
    }
    if (selected == objective::kNoTaskGroup) {
        selected = cheapest_group(*impl, target.nativeRow, *offer, counters, members, allowed);
    }
    if (selected != offer->taskGroup) {
        // Fenced on the costs' revision, so a newer cost pass is never overwritten.
        Intent intent{};
        intent.kind = IntentKind::assignCombatObjective;
        intent.firstRow = squad.nativeRow;
        intent.secondRow = target.nativeRow;
        intent.entryIndex = selected;
        intent.expectedObjectiveRevision = offer->costs.revision;
        static_cast<void>(queue_intent(state, active_frame(state), intent));
        lua_pop(state, 1);
    }
    if (selected == objective::kNoTaskGroup) {
        lua_pushnil(state);
    } else {
        push_task_group(state, target.nativeRow, selected);
    }
    lua_pushboolean(state, true);
    return 2;
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
