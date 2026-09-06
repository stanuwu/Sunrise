#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/activity_sdk/generated_world/format.h"
#include "../../../state/activity_sdk/generation/definition.h"
#include "../../../state/build_data/scriptables/definition.h"

namespace sunrise::client::content::activity::sdk_generation {

/** Result of one explicit synchronous build into an isolated artifact tree. */
enum class OfflineBuildStatus : std::uint8_t {
    ready,
    cancelled,
    busy,
    invalidInput,
    failed,
};

/** Bounded progress event emitted at the same points as the in-client worker state. */
struct OfflineBuildProgress final {
    state::activity_sdk::generation::Status status{};
    std::uint32_t current{};
    std::uint32_t total{};
    std::uint32_t scenarioTag{};
    std::string_view detail{};
};

using OfflineProgressSink = void (*)(void* context, const OfflineBuildProgress& progress) noexcept;
using OfflineCancelProbe = bool (*)(void* context) noexcept;

/** Explicit inputs required by the package-backed native generation pass. */
struct OfflineBuildRequest final {
    state::activity_sdk::generated_world::Digest sourceFingerprint{};
    const middleware::content::packages::reader::BlockKeys* keys{};
    std::wstring_view packageDirectory{};
    std::wstring_view cacheArtifactDirectory{};
    std::wstring_view outputArtifactDirectory{};
};

/** Identity and counts returned with one complete isolated tree. */
struct OfflineBuildResult final {
    std::uint32_t scenarioCount{};
    std::uint32_t activityRootCount{};
    std::uint32_t activityCount{};
    std::uint32_t builtScenarioCount{};
    std::uint32_t reusedScenarioCount{};
    state::activity_sdk::generated_world::Digest payloadSha256{};
    std::uint64_t packBytes{};
    std::uint32_t luaFiles{};
};

/** Runs the exact worker pipeline with one borrowed in-process or synthetic reader source. */
[[nodiscard]] OfflineBuildStatus build_offline(const OfflineBuildRequest& request,
                                               OfflineCancelProbe cancel,
                                               void* cancelContext,
                                               OfflineProgressSink progress,
                                               void* progressContext,
                                               OfflineBuildResult& output) noexcept;

/** Immutable boot policy for the live generator. Its pack backs host roster mission seeds. */
struct Policy final {
    bool enabled{};
    /** Writes the sdk/lua declaration tree, which no runtime loads. */
    bool luaDeclarations{};
};

/** Stores the artifact paths and applies the immutable boot policy. */
void initialize(void* module, const Policy& policy) noexcept;

/** Starts or reaps the optional full-estate package reader without blocking the caller. */
void service() noexcept;

/** Loads one tag-bound published shard even when generation is disabled. */
[[nodiscard]] bool
load_cached_scenario(std::uint32_t scenarioTag,
                     std::string_view scenarioName,
                     std::shared_ptr<state::build_data::scriptables::Snapshot>& output) noexcept;

/** Cancels and joins generation before clearing its paths and job state. */
void reset() noexcept;

} // namespace sunrise::client::content::activity::sdk_generation
