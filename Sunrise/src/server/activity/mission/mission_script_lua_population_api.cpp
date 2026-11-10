#include <algorithm>
#include <array>
#include <cstdint>

#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// One cohort holds the squad count the table allows; the metatable locks its shape.
constexpr std::size_t kCohortCapacity = static_cast<std::size_t>(api::kCohortCapacity);
constexpr char kCohortMetatable[] = "sunrise.mission.cohort";
// The table entry of context:cohort.
constexpr const api::MemberSpec& kCohort = api::member(api::Receiver::context, "cohort");

/** The handle stores SDK rows, while native Mission State owns all population facts. */
struct CohortHandle final {
    std::array<std::uint32_t, kCohortCapacity> rows{};
    std::uint8_t count{};
};

/** The aggregate every cohort member reads. */
struct CohortTotals final {
    std::int64_t alive{};
    bool known{true};
    bool full{true};
    bool observedAlive{};
    std::uint8_t size{};
};

/** Cohort members in table order. */
constexpr std::array<Binding<CohortTotals>, std::size(api::kCohortMembers)> kCohortBindings{{
    {"alive_count",
     nullptr,
     [](lua_State* state, const CohortTotals& totals) {
         if (totals.known) {
             lua_pushinteger(state, totals.alive);
         } else {
             lua_pushnil(state);
         }
     }},
    {"observed_full",
     nullptr,
     [](lua_State* state, const CohortTotals& totals) {
         lua_pushboolean(state, totals.known && totals.full);
     }},
    {"observed_alive",
     nullptr,
     [](lua_State* state, const CohortTotals& totals) {
         if (totals.known) {
             lua_pushboolean(state, totals.observedAlive);
         } else {
             lua_pushnil(state);
         }
     }},
    {"cleared",
     nullptr,
     [](lua_State* state, const CohortTotals& totals) {
         lua_pushboolean(state, totals.known && totals.full && totals.alive == 0);
     }},
    {"size",
     nullptr,
     [](lua_State* state, const CohortTotals& totals) { lua_pushinteger(state, totals.size); }},
}};
static_assert(bound_to<CohortTotals>(api::kCohortMembers, kCohortBindings));

/** Aggregates only the current attempt's exact transported squad generations. */
int cohort_index(lua_State* state) {
    const auto* handle =
        static_cast<const CohortHandle*>(luaL_checkudata(state, 1, kCohortMetatable));
    const auto* impl = impl_from_state(state);
    std::int64_t alive = 0;
    bool known = true;
    bool full = true;
    bool observedAlive = false;
    for (std::size_t index = 0; index < handle->count; ++index) {
        SquadDefinition squad{};
        if (!current_squad(state, SquadHandle{handle->rows[index]}, squad)) {
            return luaL_error(state, "cohort contains a stale squad");
        }
        const auto pending = [&squad, impl](const auto& intent) noexcept {
            const bool scene = intent.kind == IntentKind::activateAuthoredScene && intent.active
                               && intent.burstRowCount <= intent.burstRows.size()
                               && std::find(intent.burstRows.begin(),
                                            intent.burstRows.begin() + intent.burstRowCount,
                                            squad.nativeRow)
                                      != intent.burstRows.begin() + intent.burstRowCount;
            const bool source =
                scene
                || (intent.kind == IntentKind::placeSquad
                        ? intent.firstRow == squad.nativeRow
                        : intent.kind == IntentKind::runActorProgram && intent.active
                              && intent.secondRow == squad.nativeRow);
            return source && intent.attemptGeneration == impl->attempt.generation;
        };
        const auto& candidate = active_frame(state).candidate.intents;
        const auto outbox = std::span(impl->outbox).subspan(impl->outboxRead);
        if (std::any_of(candidate.begin(), candidate.end(), pending)
            || std::any_of(outbox.begin(), outbox.end(), pending)) {
            known = full = false;
            continue;
        }
        const auto found =
            std::find_if(impl->squadPopulations.begin(),
                         impl->squadPopulations.end(),
                         [&squad, impl](const auto& population) noexcept {
                             return population.squadRow == squad.nativeRow
                                    && population.attemptGeneration == impl->attempt.generation;
                         });
        if (found == impl->squadPopulations.end() || !found->generationKnown
            || !found->aliveKnown) {
            known = full = false;
            continue;
        }
        alive += found->alive;
        observedAlive = observedAlive || found->maximumObservedAlive > 0;
        // Members ever created also prove a full spawn, so an early death cannot hide one.
        full = full && found->expectedAlive > 0
               && (found->maximumObservedAlive >= found->expectedAlive
                   || found->maximumObservedCreated >= found->expectedAlive);
    }
    const CohortTotals totals{alive, known, full, observedAlive, handle->count};
    return dispatch<CohortTotals>(state, kCohortBindings, lua_string_view(state, 2), totals);
}

} // namespace

/** Resolves a nonempty group of SDK squads without exposing count bookkeeping to Lua. */
int context_cohort(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kCohort);
    lua_getfield(state, 2, required_key(kCohort, "squads", api::ValueKind::list));
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    if (count == 0 || count > kCohortCapacity) {
        return luaL_error(
            state, "cohort needs one to %d squads", static_cast<int>(kCohortCapacity));
    }
    std::size_t entries = 0;
    lua_pushnil(state);
    while (lua_next(state, list) != 0) {
        if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1
            || static_cast<std::uint64_t>(lua_tointeger(state, -2)) > count) {
            return luaL_error(state, "cohort squads must be a dense list");
        }
        ++entries;
        lua_pop(state, 1);
    }
    if (entries != count) {
        return luaL_error(state, "cohort squads must be a dense list");
    }
    CohortHandle cohort{};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        const auto* handle =
            static_cast<const SquadHandle*>(luaL_testudata(state, -1, kSquadMetatable));
        SquadDefinition squad{};
        if (!(handle != nullptr ? current_squad(state, *handle, squad)
                                : resolve_squad(state, -1, squad))) {
            return luaL_error(state, "cohort requires an SDK squad declaration");
        }
        lua_pop(state, 1);
        const auto previous = std::span(cohort.rows).first(index);
        if (std::find(previous.begin(), previous.end(), squad.localRow) != previous.end()) {
            return luaL_error(state, "cohort repeats a squad");
        }
        cohort.rows[index] = squad.localRow;
    }
    cohort.count = static_cast<std::uint8_t>(count);
    push_handle(state, kCohortMetatable, cohort);
    return 1;
}

void register_population_metatables(lua_State* state) {
    register_metatable(state, kCohortMetatable, &cohort_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
