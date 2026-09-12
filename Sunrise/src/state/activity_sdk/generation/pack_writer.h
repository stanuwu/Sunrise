#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../format.h"

namespace sunrise::state::activity_sdk::generation::pack {

/** One SHA-256 value used by the canonical pack envelope. */
using Digest = std::array<std::byte, 32>;

/** The three generated-source identities stored in every runtime-pack header. */
struct Identity final {
    Digest sdkBuildSha256{};
    Digest contentKeySha256{};
    Digest logicalIrSha256{};
};

/** Borrowed canonical tables in the exact current section order. */
struct Tables final {
    std::span<const std::byte> strings{};
    std::span<const format::Activity> activities{};
    std::span<const format::Scenario> scenarios{};
    std::span<const format::Bubble> bubbles{};
    std::span<const format::State> states{};
    std::span<const format::Object> objects{};
    std::span<const format::Occurrence> occurrences{};
    std::span<const format::Slot> slots{};
    std::span<const format::Text> texts{};
    std::span<const format::Capability> capabilities{};
    std::span<const format::Gate> gates{};
    std::span<const format::Refusal> refusals{};
    std::span<const format::ActorClass> actorClasses{};
    std::span<const format::RsatDescriptor> rsatDescriptors{};
    std::span<const format::RsatSchema> rsatSchemas{};
    std::span<const format::RsatField> rsatFields{};
    std::span<const format::Squad> squads{};
    std::span<const format::SquadMember> squadMembers{};
    std::span<const format::SquadAnchor> squadAnchors{};
    std::span<const format::AuthoredSceneResource> authoredSceneResources{};
    std::span<const format::AuthoredSceneSquadEdge> authoredSceneSquadEdges{};
    std::span<const format::TaskTarget> taskTargets{};
    std::span<const format::DialogueCueText> dialogueCueTexts{};
    std::span<const format::DirectiveElement> directiveElements{};
    std::span<const format::ActivityBindingTag> activityBindingTags{};
    std::span<const format::ActivityBindingLocator> activityBindingLocators{};
    std::span<const format::BehaviorProgram> behaviorPrograms{};
    std::span<const format::BehaviorInput> behaviorInputs{};
    std::span<const format::BehaviorChannelWrite> behaviorChannelWrites{};
    std::span<const format::BehaviorOwner> behaviorOwners{};
    std::span<const format::BehaviorActivityBinding> behaviorActivityBindings{};
    std::span<const format::ActorMessageSchema> actorMessageSchemas{};
    std::span<const format::ActorCommandDefinition> actorCommandDefinitions{};
    std::span<const format::ActorBehaviorProfile> actorBehaviorProfiles{};
    std::span<const format::SimulationEventDefinition> simulationEventDefinitions{};
    std::span<const format::RuntimeSchema> runtimeSchemas{};
    std::span<const format::RuntimeField> runtimeFields{};
    std::span<const format::SobjectRsat> sobjectRsats{};
    std::span<const format::SobjectRsatDescriptor> sobjectRsatDescriptors{};
    std::span<const format::EntityTypeDefinition> entityTypeDefinitions{};
    std::span<const format::SobjectRsatFieldBinding> sobjectRsatFieldBindings{};
    std::span<const format::RuntimeTypeDefinition> runtimeTypeDefinitions{};
    std::span<const format::ActorStateName> actorStateNames{};
    std::span<const format::ActorSequenceTable> actorSequenceTables{};
    std::span<const format::ActorSequenceEntry> actorSequenceEntries{};
    std::span<const format::ActorSequenceBinding> actorSequenceBindings{};
};

/** Canonical pack build and publication outcomes. */
enum class Status : std::uint8_t {
    ready,
    invalidInput,
    sizeOverflow,
    hashFailure,
    invalidPack,
    ioFailure,
};

/**
 * One identity-independent pack image produced by `prepare` and consumed by `publish`.
 * Its bytes stay private so the publication path can trust them without hashing them again.
 */
class PreparedImage final {
public:
    PreparedImage() noexcept = default;
    PreparedImage(const PreparedImage&) = delete;
    PreparedImage& operator=(const PreparedImage&) = delete;
    PreparedImage(PreparedImage&&) noexcept = default;
    PreparedImage& operator=(PreparedImage&&) noexcept = default;

    /** @return Exact serialized file size, including the header. */
    [[nodiscard]] std::uint64_t file_size() const noexcept {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    /** @return Digest of the identity-independent serialized payload. */
    [[nodiscard]] const Digest& payload_sha256() const noexcept {
        return payloadSha256_;
    }

private:
    friend Status prepare(const Tables& tables, PreparedImage& output) noexcept;
    friend Status publish(const wchar_t* path,
                          const Identity& identity,
                          PreparedImage prepared,
                          Digest& payloadSha256) noexcept;
    friend Status build(const Identity& identity,
                        const Tables& tables,
                        std::vector<std::byte>& output,
                        Digest& payloadSha256) noexcept;

    std::vector<std::byte> bytes_{};
    Digest payloadSha256_{};
};

/**
 * Builds and hashes one identity-independent runtime-pack image exactly once.
 * The final header identity can be derived from `payload_sha256` before publication.
 * @param tables Borrowed string bytes and fixed-layout rows.
 * @param output Replaced only with a complete deterministic image.
 * @return Precise build outcome.
 */
[[nodiscard]] Status prepare(const Tables& tables, PreparedImage& output) noexcept;

/**
 * Binds the final identity and publishes the exact bytes returned by `prepare`.
 * The writer checks exact writes, close, and atomic rename. It does not reread trusted bytes;
 * `validate` and `validate_file` remain the boundaries for persisted input.
 * @param path Null-terminated final pack path.
 * @param identity Final source identities derived from the prepared payload.
 * @param prepared One-shot image returned by `prepare`.
 * @param payloadSha256 Receives the committed payload digest only on success.
 * @return Precise publication outcome.
 */
[[nodiscard]] Status publish(const wchar_t* path,
                             const Identity& identity,
                             PreparedImage prepared,
                             Digest& payloadSha256) noexcept;

/**
 * Builds one deterministic SRSDKP01 runtime-pack file image.
 * Input row order and string offsets are already canonical and are copied without translation.
 * @param identity Exact source identities for this generated estate.
 * @param tables Borrowed string bytes and fixed-layout rows.
 * @param output Replaced only with a complete deterministic image.
 * @param payloadSha256 Receives the payload digest only on success.
 * @return `ready` only for a complete image.
 */
[[nodiscard]] Status build(const Identity& identity,
                           const Tables& tables,
                           std::vector<std::byte>& output,
                           Digest& payloadSha256) noexcept;

/**
 * Validates one complete runtime-pack envelope without applying catalog relation rules.
 * This is the explicit buffer boundary for persisted or otherwise untrusted input.
 * @param bytes Complete header and payload bytes.
 * @param identity Expected source identities.
 * @param payloadSha256 Receives the verified payload digest only on success.
 * @return True when the header, sections, identities, and payload hash all match.
 */
[[nodiscard]] bool validate(std::span<const std::byte> bytes,
                            const Identity& identity,
                            Digest& payloadSha256) noexcept;

/** Reopens and authenticates one complete runtime-pack file. */
[[nodiscard]] bool
validate_file(const wchar_t* path, const Identity& identity, Digest& payloadSha256) noexcept;

/**
 * Publishes one complete pack through a writer-owned sibling.
 * The parent directory must already exist.
 * @param path Null-terminated final pack path.
 * @param identity Exact source identities for this generated estate.
 * @param tables Borrowed string bytes and fixed-layout rows.
 * @param payloadSha256 Receives the committed payload digest only on success.
 * @return `ready` only after exact write, close, and atomic rename succeed.
 */
[[nodiscard]] Status write(const wchar_t* path,
                           const Identity& identity,
                           const Tables& tables,
                           Digest& payloadSha256) noexcept;

} // namespace sunrise::state::activity_sdk::generation::pack
