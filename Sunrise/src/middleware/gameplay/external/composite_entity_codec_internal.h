#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "../../encoding/bit_raw.h"
#include "composite_entity_codec.h"

/** Reflection primitives shared by the composite entity codec sources. */
namespace sunrise::middleware::gameplay::external::detail {

namespace format = state::activity_sdk::format;
namespace bits = middleware::encoding::bits;

/** Marks a retained mirror body so another callback's layout cannot be read as one. */
inline constexpr std::uint32_t kMirrorIdentity = 0x4345324DU;

/** Private header distinguishes retained mirror bodies from other payload layouts. */
struct MirrorHeader final {
    std::uint32_t identity{kMirrorIdentity};
    std::uint32_t semanticTag{};
    std::uint16_t bitCount{};
    std::uint16_t reserved{};
};

/** One validated header and its exact body bytes. */
struct MirrorView final {
    MirrorHeader header{};
    std::span<const std::byte> bytes{};
};

/** One reflection walk is bound to an SDK view and codec family. */
struct ResolverContext final {
    const CompositeEntityCodecContext* catalog{};
    format::RuntimeCodecFamily family{format::RuntimeCodecFamily::activity};
    std::span<std::uint8_t> presence{};
    std::uint32_t firstFieldBit{};
    std::uint32_t componentTag{};
};

/** Builds one bounded canonical body while the input is decoded. */
class MirrorBuilder final {
public:
    state::gameplay::entity_identity::ActorSourceReference actorSource{};
    std::uint16_t damageHealth{};
    std::uint16_t damageShield{};
    bool damageKnown{};
    /** Damage pools seen so far; each pool is its own nested walk. */
    std::uint8_t damagePools{};
    /** Opens an empty fixed-capacity bit writer. */
    MirrorBuilder() noexcept : writer_(bytes_) {}

    /** Appends one scalar field. */
    [[nodiscard]] bool append(std::uint64_t value, std::uint8_t width) noexcept {
        return writer_.write(value, width);
    }

    /** Appends an exact meaningful-bit prefix from byte storage. */
    [[nodiscard]] bool append(std::span<const std::byte> bytes, std::size_t bitCount) noexcept {
        bits::Reader reader(bytes);
        return bits::copy(reader, writer_, bitCount);
    }

    /** Seals the canonical body into callback-owned payload state. */
    [[nodiscard]] bool finish(std::uint32_t semanticTag, TypePayload& output) noexcept {
        std::size_t byteCount = 0;
        if (!writer_.finish(byteCount) || writer_.bit_count() > kMaximumTypePayloadBits
            || byteCount + sizeof(MirrorHeader) > output.state.size()) {
            return false;
        }
        MirrorHeader header{};
        header.semanticTag = semanticTag;
        header.bitCount = static_cast<std::uint16_t>(writer_.bit_count());
        TypePayload candidate{};
        std::memcpy(candidate.state.data(), &header, sizeof(header));
        std::memcpy(candidate.state.data() + sizeof(header), bytes_.data(), byteCount);
        candidate.byteCount = static_cast<std::uint16_t>(sizeof(header) + byteCount);
        candidate.actorSource = actorSource;
        candidate.damageHealth = damageHealth;
        candidate.damageShield = damageShield;
        candidate.damageKnown = damageKnown;
        output = candidate;
        return true;
    }

private:
    std::array<std::byte, kMaximumTypePayloadBits / 8U> bytes_{};
    bits::Writer writer_;
};

/** Finds one unique runtime schema handle in the pinned view. */
[[nodiscard]] const format::RuntimeSchema*
runtime_schema(const CompositeEntityCodecContext& context, std::uint32_t handle) noexcept;

/** Finds one installed RSAT in its generated tag order. */
[[nodiscard]] const format::SobjectRsat* sobject_rsat(const CompositeEntityCodecContext& context,
                                                      std::uint32_t tag) noexcept;

/** Checks the complete 17-bit entity-token domain. */
[[nodiscard]] bool valid_token(const EntityToken& token) noexcept;

/** Validates and opens one callback-owned canonical mirror. */
[[nodiscard]] bool load_mirror(const TypePayload& payload, MirrorView& output) noexcept;

/** Reads one field and retains the same wire bits. */
[[nodiscard]] bool read_and_append(bits::Reader& reader,
                                   MirrorBuilder& mirror,
                                   std::uint8_t width,
                                   std::uint64_t& output) noexcept;

/** Reads and retains one required boolean field. */
[[nodiscard]] bool read_flag(bits::Reader& reader, MirrorBuilder& mirror, bool& output) noexcept;

/** Decodes, re-encodes, and retains one complete reflected schema body. */
[[nodiscard]] bool append_schema(bits::Reader& reader,
                                 MirrorBuilder& mirror,
                                 ResolverContext& resolverContext,
                                 std::uint32_t schemaHandle,
                                 std::uint32_t* semanticTag) noexcept;

/** Resolves one exact channel-2 type contract from the SDK. */
[[nodiscard]] const format::EntityTypeDefinition*
entity_definition(const CompositeEntityCodecContext& context, EntityType type) noexcept;

/** Pins one authenticated SDK snapshot and resets stale registry ownership. */
[[nodiscard]] bool bind_context(CompositeEntityCodecContext& context,
                                EntityBaselineRegistry& registry,
                                const state::activity_sdk::Snapshot& catalog,
                                SobjectPositionCompression positionCompression) noexcept;

/** Resolves an update-only entity type from its session registry. */
[[nodiscard]] bool
resolve_type(const void* raw, const EntityToken& token, EntityType& output) noexcept;

/** TypePayloadCodec reader adapter with no registry mutation. */
[[nodiscard]] bool read_payload(const void* raw,
                                const EntityToken& token,
                                EntityType type,
                                TypePayloadPart part,
                                const TypePayload* baseline,
                                bits::Reader& reader,
                                TypePayload& output) noexcept;

/** Revalidates a mirror through reflection before replaying it. */
[[nodiscard]] bool write_payload(const void* raw,
                                 const EntityToken& token,
                                 EntityType type,
                                 TypePayloadPart part,
                                 const TypePayload* baseline,
                                 const TypePayload& payload,
                                 bits::Writer& writer) noexcept;

/** A cell profile is borrowed for one body and never changes another record's codec context. */
[[nodiscard]] bool read_cell_payload(const void* raw,
                                     const EntityToken& token,
                                     EntityType type,
                                     TypePayloadPart part,
                                     const TypePayload* baseline,
                                     std::uint16_t cell,
                                     bits::Reader& reader,
                                     TypePayload& output) noexcept;

/** Replay selects the same package cell profile used to decode the retained body. */
[[nodiscard]] bool write_cell_payload(const void* raw,
                                      const EntityToken& token,
                                      EntityType type,
                                      TypePayloadPart part,
                                      const TypePayload* baseline,
                                      const TypePayload& payload,
                                      std::uint16_t cell,
                                      bits::Writer& writer) noexcept;

/** Finds or creates one bounded session without evicting another session. */
[[nodiscard]] CompositeEntitySession*
session(CompositeEntitySessionStore& store,
        std::uint64_t groupSessionId,
        bool create,
        const state::gameplay::entity_identity::Source& source = {}) noexcept;

} // namespace sunrise::middleware::gameplay::external::detail
