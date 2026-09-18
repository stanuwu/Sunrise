#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "activity_sdk_lua_missions_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {

/** Appends one Lua string literal without depending on a runtime serializer. */
void append_string(std::string& output, std::string_view value) {
    output.push_back('"');
    // Escaped bytes are emitted as uppercase hex.
    constexpr char digits[] = "0123456789ABCDEF";
    for (const unsigned char byte : value) {
        switch (byte) {
        case '\\':
            output.append("\\\\");
            break;
        case '"':
            output.append("\\\"");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            if (byte < 0x20U || byte == 0x7FU) {
                output.append("\\x");
                output.push_back(digits[byte >> 4U]);
                output.push_back(digits[byte & 0xFU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

/** Converts arbitrary extracted names to stable Lua identifiers or safe file stems. */
std::string identifier(std::string_view value, bool upper) {
    std::string output;
    output.reserve(value.size() + 1U);
    bool separator = false;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) != 0) {
            if (separator && !output.empty()) {
                output.push_back('_');
            }
            separator = false;
            output.push_back(static_cast<char>(upper ? std::toupper(byte) : std::tolower(byte)));
        } else {
            separator = true;
        }
    }
    while (!output.empty() && output.back() == '_') {
        output.pop_back();
    }
    if (output.empty()) {
        output = upper ? "UNNAMED" : "unnamed";
    }
    if (std::isdigit(static_cast<unsigned char>(output.front())) != 0) {
        output.insert(output.begin(), '_');
    }
    return output;
}

void append_hex(std::string& output, std::uint32_t value) {
    std::array<char, 16> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "0x%08X", value);
    output.append(buffer.data(), static_cast<std::size_t>(length));
}

void append_uint(std::string& output, std::uint32_t value) {
    std::array<char, 16> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "%u", value);
    output.append(buffer.data(), static_cast<std::size_t>(length));
}

std::string stem(std::string_view name, std::uint32_t identity) {
    std::string output = identifier(name, false);
    std::array<char, 16> suffix{};
    const int length = std::snprintf(suffix.data(), suffix.size(), "_%08x", identity);
    output.append(suffix.data(), static_cast<std::size_t>(length));
    return output;
}

bool range_inside(format::Range range, std::size_t size) noexcept {
    return range.first <= size && range.count <= size - range.first;
}

/** Keeps the extracted name unchanged unless another row already owns that Lua key. */
std::string append_unique_key(std::string& output,
                              std::unordered_set<std::string>& used,
                              std::string_view name,
                              std::uint32_t fallback) {
    std::string key = identifier(name, true);
    if (!used.insert(key).second) {
        std::array<char, 16> suffix{};
        const int length = std::snprintf(suffix.data(), suffix.size(), "_%08X", fallback);
        key.append(suffix.data(), static_cast<std::size_t>(length));
        (void)used.insert(key);
    }
    output.append("    ");
    output.append(key);
    output.append(" = ");
    return key;
}

namespace {

[[nodiscard]] std::string_view activity_name(const Source& source,
                                             const format::Activity& activity) noexcept {
    const std::string_view internal = text(source, activity.internalName);
    return internal.empty() ? text(source, activity.displayName) : internal;
}

/** Builds immutable lookup rows once for the whole Lua pass. */
[[nodiscard]] bool build_render_index(const Source& source, RenderIndex& output) {
    output = {};
    output.squadsByScenario.resize(source.scenarios.size());
    for (std::uint32_t index = 0; index < source.squads.size(); ++index) {
        const format::Squad& squad = source.squads[index];
        if (squad.scenarioIndex >= output.squadsByScenario.size()) {
            return false;
        }
        output.squadsByScenario[squad.scenarioIndex].push_back(index);
    }
    output.scenesBySlot.reserve(source.authoredSceneResources.size());
    for (std::uint32_t index = 0; index < source.authoredSceneResources.size(); ++index) {
        output.scenesBySlot[source.authoredSceneResources[index].slotIndex].push_back(index);
    }
    output.tasksBySlot.reserve(source.taskTargets.size());
    for (std::uint32_t index = 0; index < source.taskTargets.size(); ++index) {
        output.tasksBySlot[source.taskTargets[index].taskSlotIndex].push_back(index);
    }
    for (std::uint32_t index = 0; index < source.dialogueCues.size(); ++index) {
        output.dialogueCuesBySlot.emplace(source.dialogueCues[index].slotIndex, index);
    }
    for (std::uint32_t index = 0; index < source.dialogueCueTexts.size(); ++index) {
        output.dialogueBySlot[source.dialogueCueTexts[index].slotIndex].push_back(index);
    }
    for (std::uint32_t index = 0; index < source.directiveElements.size(); ++index) {
        output.directivesBySlot[source.directiveElements[index].slotIndex].push_back(index);
    }
    for (std::uint32_t index = 0; index < source.combatObjectiveGroups.size(); ++index) {
        output.combatGroupsBySlot[source.combatObjectiveGroups[index].slotIndex].push_back(index);
    }
    for (std::uint32_t index = 0; index < source.actorAbilities.size(); ++index) {
        output.abilitiesBySlot[source.actorAbilities[index].slotIndex].push_back(index);
    }
    output.worldsByScenarioTag.reserve(source.scenarioWorldSources.size());
    for (std::uint32_t index = 0; index < source.scenarioWorldSources.size(); ++index) {
        if (!output.worldsByScenarioTag
                 .emplace(source.scenarioWorldSources[index].scenarioTag, index)
                 .second) {
            return false;
        }
    }
    return true;
}

/** Shared read-only state for deterministic indexed mission rendering. */
struct MissionRenderBatch final {
    const Source* source{};
    const RenderIndex* index{};
    std::vector<SourceModule>* modules{};
    std::atomic_size_t next{};
    std::atomic_bool failed{};
};

/** Uses most cores while leaving two for the running game and server. */
[[nodiscard]] std::size_t render_worker_count(std::size_t rows) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::size_t processors = static_cast<std::size_t>(info.dwNumberOfProcessors);
    const std::size_t available = processors > 2U ? processors - 2U : 1U;
    return (std::min)(rows, (std::min)(available, static_cast<std::size_t>(12)));
}

/** Renders independent mission modules into their fixed canonical rows. */
void run_mission_render_worker(MissionRenderBatch& batch) noexcept {
    while (!batch.failed.load(std::memory_order_relaxed)) {
        const std::size_t row = batch.next.fetch_add(1U);
        if (row >= batch.modules->size()) {
            break;
        }
        try {
            if (!render_mission(*batch.source,
                                *batch.index,
                                static_cast<std::uint32_t>(row),
                                batch.source->scenarios[row],
                                (*batch.modules)[row])) {
                batch.failed.store(true, std::memory_order_relaxed);
            }
        } catch (...) {
            batch.failed.store(true, std::memory_order_relaxed);
        }
    }
}

/** Adapts one mission renderer to the Windows thread ABI. */
DWORD WINAPI mission_render_thread_main(void* opaque) noexcept {
    run_mission_render_worker(*static_cast<MissionRenderBatch*>(opaque));
    return 0;
}

/** Renders all mission modules in parallel without changing output order. */
[[nodiscard]] bool
render_missions(const Source& source, const RenderIndex& index, std::vector<SourceModule>& output) {
    output.clear();
    output.resize(source.scenarios.size());
    MissionRenderBatch batch{&source, &index, &output};
    const std::size_t workers = render_worker_count(output.size());
    if (workers == 0) {
        return false;
    }
    std::vector<HANDLE> threads;
    threads.reserve(workers - 1U);
    for (std::size_t worker = 1; worker < workers; ++worker) {
        const HANDLE thread =
            CreateThread(nullptr, 0, &mission_render_thread_main, &batch, 0, nullptr);
        if (thread != nullptr) {
            threads.push_back(thread);
        }
    }
    run_mission_render_worker(batch);
    for (const HANDLE thread : threads) {
        (void)WaitForSingleObject(thread, INFINITE);
        (void)CloseHandle(thread);
    }
    return !batch.failed.load(std::memory_order_relaxed);
}

/** Emits one small activity module that names its concrete mission source. */
[[nodiscard]] bool render_activity(const Source& source,
                                   const format::Activity& activity,
                                   std::span<const SourceModule> missions,
                                   SourceModule& module) {
    const std::string_view name = activity_name(source, activity);
    module.stem = stem(name, activity.definitionHash);
    std::string output = "-- Generated from installed activity data. Do not edit.\n"
                         "local sdk = require(\"sunrise.activity_sdk\")\n\n"
                         "---@type SunriseActivity\n"
                         "local activity = {\n    name = ";
    append_string(output, name);
    output.append(",\n    display_name = ");
    append_string(output, text(source, activity.displayName));
    output.append(",\n    id = ");
    append_string(output, text(source, activity.id));
    output.append(",\n    index = ");
    append_uint(output, activity.activityIndex);
    output.append(",\n    definition_hash = ");
    append_hex(output, activity.definitionHash);
    output.append(",\n    activity_root_tag = ");
    append_hex(output, activity.selectedActivityRootTag);
    output.append(",\n    scenario_tag = ");
    append_hex(output, activity.selectedScenarioTag);
    output.append(",\n    matchmaking_config_tag = ");
    append_hex(output, activity.matchmakingConfigTag);
    if (activity.scenarioIndex < missions.size()) {
        output.append(",\n    mission = require(\"missions.");
        output.append(missions[activity.scenarioIndex].stem);
        output.append("\")");
    }
    output.append(",\n}\n\nreturn activity\n");
    module.source = std::move(output);
    return true;
}

} // namespace

/** Builds readable activity and mission modules before replacing either bundle field. */
bool render_activity_files(const Source& source, Bundle& output) noexcept {
    try {
        RenderIndex renderIndex{};
        std::vector<SourceModule> missions;
        if (!build_render_index(source, renderIndex)
            || !render_missions(source, renderIndex, missions)) {
            return false;
        }
        std::string missionIndex = "-- Generated mission module names. Do not edit.\nreturn {\n";
        std::unordered_set<std::string> missionKeys;
        for (std::size_t index = 0; index < source.scenarios.size(); ++index) {
            const format::Scenario& scenario = source.scenarios[index];
            const SourceModule& module = missions[index];
            (void)append_unique_key(
                missionIndex, missionKeys, text(source, scenario.name), scenario.tag);
            append_string(missionIndex, "missions." + module.stem);
            missionIndex.append(",\n");
        }
        missionIndex.append("}\n");

        std::vector<SourceModule> activities;
        activities.reserve(source.activities.size());
        std::string activityIndex = "-- Generated activity module names. Do not edit.\nreturn {\n";
        std::unordered_set<std::string> activityKeys;
        for (const format::Activity& activity : source.activities) {
            SourceModule module{};
            if (!render_activity(source, activity, missions, module)) {
                return false;
            }
            (void)append_unique_key(activityIndex,
                                    activityKeys,
                                    activity_name(source, activity),
                                    activity.definitionHash);
            append_string(activityIndex, "activities." + module.stem);
            activityIndex.append(",\n");
            activities.push_back(std::move(module));
        }
        activityIndex.append("}\n");
        output.activityIndex = std::move(activityIndex);
        output.missionIndex = std::move(missionIndex);
        output.activityModules = std::move(activities);
        output.missionModules = std::move(missions);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
