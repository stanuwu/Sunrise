#pragma once

#include <cstdint>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::activity::sdk_generation::behavior_inventory {

/** One installed object-behavior root and its contiguous input and write ranges. */
struct Program final {
    std::uint32_t rootTag{};
    std::uint32_t firstInput{};
    std::uint32_t inputCount{};
    std::uint32_t firstWrite{};
    std::uint32_t writeCount{};
    std::uint32_t nodeCount{};
    std::uint32_t expressionCount{};
};

/** One exact compiled-expression input resolved against an object-local channel. */
struct Input final {
    std::uint32_t programRow{};
    std::uint32_t nodeOffset{};
    std::uint32_t expressionOffset{};
    std::uint32_t channelHash{};
    std::uint64_t inputOrMode{};
    std::int32_t nativeOverride{};
    std::uint32_t activeField{};
    std::uint8_t selector{};
    std::uint8_t role{};
};

/** One storage-channel action and the local channel it writes. */
struct ChannelWrite final {
    std::uint32_t programRow{};
    std::uint32_t nodeOffset{};
    std::uint32_t channelHash{};
};

enum class SubmissionKind : std::uint32_t {
    activeNative,
    passive,
    unresolved,
};

/** One exact root reference owned by an activity-reached actor definition. */
struct Owner final {
    std::uint32_t programRow{};
    std::uint32_t actorClassIndex{};
    std::uint32_t configTag{};
    std::uint32_t configFieldOffset{};
    std::uint32_t buildOrdinal{};
    std::uint32_t descriptorOrdinal{};
    std::uint32_t submitterSubtype{};
    SubmissionKind submissionKind{SubmissionKind::unresolved};
};

/** Full installed object-behavior root inventory. */
struct Snapshot final {
    std::vector<Program> programs{};
    std::vector<Input> inputs{};
    std::vector<ChannelWrite> writes{};
    std::vector<Owner> owners{};
    std::uint32_t parsedPrograms{};
    std::uint32_t unparsedPrograms{};
    std::uint32_t unreadPrograms{};
    std::uint32_t skippedActors{};
    std::uint32_t unreadActors{};
    std::uint32_t unreadConfigs{};
    bool ready{};
};

using CancelProbe = bool (*)(void*) noexcept;

/** Reads every installed object-behavior root once and retains its exact channel edges. */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         std::span<const std::uint32_t> actorTags,
                         CancelProbe cancel,
                         void* cancelContext,
                         Snapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::behavior_inventory
