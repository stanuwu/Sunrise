#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_80807d.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"
#include "squad_auth_body.h"

// Type-2 paths, abilities, referenced-slot registration and retirement share one root prefix.

namespace sunrise::middleware::bap::activity_message::combatant_auth {

namespace fields = auth_fields;
namespace generated = middleware::content::packages::schema::generated;

// Actor control bodies use the Type 2 Auth schema.
inline constexpr std::uint32_t kSchema = scriptable_auth::kType2Schema;
/** Root .1 and .2 encode logical zero with bias one: recreate and own the named actor. */
inline constexpr std::uint8_t kRootModeWidth = 2;
inline constexpr std::uint8_t kRootMarkerWidth = 3;
inline constexpr std::uint32_t kRootModeValue = 1;
inline constexpr std::uint32_t kRootMarkerValue = 1;
/** A one-lane program starts at progress zero and carries count one. */
inline constexpr std::uint8_t kProgramHeaderWidth = 6;
inline constexpr std::uint32_t kProgramHeaderFirst = 0;
inline constexpr std::uint32_t kProgramHeaderSecond = 1;
/** Field .6 program kind, biased by one on the wire: 3 follows a path, 9 runs an action. */
inline constexpr std::uint8_t kProgramKindWidth = 4;
inline constexpr std::uint32_t kProgramKindBias = 1;
inline constexpr std::uint32_t kPathProgramKind = 3;
inline constexpr std::uint32_t kActionProgramKind = 9;
/** Field .6 completion selector: logical zero with bias one, the native completion. */
inline constexpr std::uint8_t kCompletionWidth = 2;
inline constexpr std::uint32_t kCompletionValue = 1;
/** A path program names a type-58 authored path and one of its two markers. */
inline constexpr std::uint32_t kPathSlotType = 58;
/** Component class a type-58 path slot carries. */
inline constexpr std::uint32_t kAbilityPointComponentClass =
    generated::AbilityPointComponent::kClass;
inline constexpr std::uint8_t kPathMarkerWidth = 8;
/** Marker 0 is the path start, marker 1 its destination. */
inline constexpr std::uint32_t kPathDestinationMarker = 1;
/** An action program names an authored group and action hash, with no spatial target. */
inline constexpr std::uint8_t kActionTargetModeWidth = 3;
inline constexpr std::uint8_t kActionTargetMarkerWidth = 8;
/** The 8-bit target marker stores -1 with a bias of 128. */
inline constexpr std::uint32_t kActionNoTargetMarker = 127;
/** Field .7 delivery manifest: a 4-bit squad count, then one type-1 squad ClientRef each. */
inline constexpr std::uint8_t kManifestCountWidth = 4;
inline constexpr std::size_t kMaximumManifestSquads = 8;
/** Bits one manifest squad adds: its ClientRef. */
inline constexpr std::size_t kManifestSquadBits = fields::kClientRefBits;

/** Fixed bit and byte counts of each body. */
inline constexpr std::size_t kPathBits = 156;
inline constexpr std::size_t kPathBytes = 20;
inline constexpr std::size_t kActionBits = 254;
inline constexpr std::size_t kActionBytes = 32;
inline constexpr std::size_t kDeliveryBits = 132;
inline constexpr std::size_t kRetireBits = 77;
inline constexpr std::size_t kRetireBytes = 10;

/** One squad the delivery manifest names. */
struct SquadReference final {
    std::uint32_t registryKey{};
    std::uint16_t squadIndex{};
};

struct PathRequest final {
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t registryKey{};
    std::uint16_t pathIndex{};
};

struct ActionRequest final {
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t group{};
    std::uint32_t action{};
};

/** @return True when a 31-bit counter is positive and representable. */
[[nodiscard]] constexpr bool valid_counter(std::uint32_t value) noexcept {
    return value != 0 && value <= fields::kMaximumCounter;
}

/** Writes the spawn revision, recreate mode, named-actor ownership and enable state. */
[[nodiscard]] inline bool
write_root(encoding::bits::Writer& writer, std::uint32_t generation, bool enabled) noexcept {
    const std::array<fields::Field, 5> root{{
        {1, fields::kPresenceWidth},
        {generation, fields::kCounterWidth},
        {kRootModeValue, kRootModeWidth},
        {kRootMarkerValue, kRootMarkerWidth},
        {enabled ? 1U : 0U, fields::kBoolWidth},
    }};
    return fields::write_fields(writer, root);
}

/** Writes the .6 program header up to and including its kind and completion selector. */
[[nodiscard]] inline bool write_program_header(encoding::bits::Writer& writer,
                                               std::uint32_t revision,
                                               std::uint32_t kind) noexcept {
    const std::array<fields::Field, 9> header{{
        {0, fields::kPresenceWidth}, // .4 absent
        {0, fields::kPresenceWidth}, // .5 absent
        {1, fields::kPresenceWidth}, // .6 present
        {revision, fields::kCounterWidth},
        {kProgramHeaderFirst, kProgramHeaderWidth},
        {kProgramHeaderSecond, kProgramHeaderWidth},
        {1, fields::kPresenceWidth},
        {kind + kProgramKindBias, kProgramKindWidth},
        {kCompletionValue, kCompletionWidth},
    }};
    return fields::write_fields(writer, header);
}

/**
 * Encodes one movement program. The spawn generation stays, the program revision advances.
 * @param output Exactly kPathBytes.
 * @return False on an out-of-range counter or index, or a size mismatch.
 */
[[nodiscard]] inline bool encode_path(const PathRequest& request,
                                      std::span<std::byte> output) noexcept {
    if (output.size() != kPathBytes || !valid_counter(request.generation)
        || !valid_counter(request.revision) || request.registryKey == 0
        || request.pathIndex > fields::kMaximumClientRefIndex) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 3> tail{{
        {kPathDestinationMarker, kPathMarkerWidth},
        {1, fields::kBoolWidth},     // follow the authored curve
        {0, fields::kPresenceWidth}, // .7 absent
    }};
    return write_root(writer, request.generation, true)
           && write_program_header(writer, request.revision, kPathProgramKind)
           && fields::write_client_ref(
               writer, request.registryKey, kPathSlotType, request.pathIndex)
           && fields::write_fields(writer, tail)
           && fields::finish_exact(writer, kPathBits, kPathBytes, written);
}

/**
 * Encodes one custom action program with no spatial target.
 * @param output Exactly kActionBytes.
 * @return False on an out-of-range counter, a zero or no-name action, or a size mismatch.
 */
[[nodiscard]] inline bool encode_action(const ActionRequest& request,
                                        std::span<std::byte> output) noexcept {
    if (output.size() != kActionBytes || !valid_counter(request.generation)
        || !valid_counter(request.revision) || request.action == 0
        || request.action == fields::kClientRefAbsentKey) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 3> identities{{
        {request.group, 32},
        {request.action, 32},
        {fields::kClientRefAbsentKey, 32}, // no additional identity
    }};
    const std::array<fields::Field, 3> tail{{
        {0, kActionTargetModeWidth},
        {kActionNoTargetMarker, kActionTargetMarkerWidth},
        {0, fields::kPresenceWidth}, // .7 absent
    }};
    return write_root(writer, request.generation, true)
           && write_program_header(writer, request.revision, kActionProgramKind)
           && fields::write_fields(writer, identities) && fields::write_absent_client_ref(writer)
           && fields::write_fields(writer, tail)
           && fields::finish_exact(writer, kActionBits, kActionBytes, written);
}

// TODO: expose a named operation once the actor registration consumer is proved.
/** Encodes a bounded, duplicate-free Type-1 reference list. */
[[nodiscard]] inline bool encode_delivery(std::uint32_t generation,
                                          std::uint32_t revision,
                                          std::span<const SquadReference> squads,
                                          std::span<std::byte> output,
                                          std::size_t& written,
                                          std::size_t& bits) noexcept {
    written = 0;
    bits = 0;
    if (!valid_counter(generation) || !valid_counter(revision) || squads.empty()
        || squads.size() > kMaximumManifestSquads) {
        return false;
    }
    for (std::size_t index = 0; index < squads.size(); ++index) {
        if (squads[index].registryKey == 0
            || squads[index].squadIndex > fields::kMaximumClientRefIndex) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (squads[index].registryKey == squads[prior].registryKey
                && squads[index].squadIndex == squads[prior].squadIndex) {
                return false;
            }
        }
    }
    const std::size_t expectedBits = kDeliveryBits + kManifestSquadBits * (squads.size() - 1);
    const std::size_t expectedBytes = (expectedBits + 7) / 8;
    if (output.size() < expectedBytes) {
        return false;
    }
    encoding::bits::Writer writer(output.first(expectedBytes));
    const std::array<fields::Field, 5> manifestHeader{{
        {0, fields::kPresenceWidth}, // .4 absent
        {0, fields::kPresenceWidth}, // .5 absent
        {0, fields::kPresenceWidth}, // .6 absent
        {1, fields::kPresenceWidth}, // .7 present
        {squads.size(), kManifestCountWidth},
    }};
    if (!write_root(writer, generation, true) || !fields::write_fields(writer, manifestHeader)) {
        return false;
    }
    for (const SquadReference& squad : squads) {
        if (!fields::write_client_ref(writer,
                                      squad.registryKey,
                                      static_cast<std::uint32_t>(squad_auth::kSlotType),
                                      squad.squadIndex)) {
            return false;
        }
    }
    if (!writer.write(revision, fields::kCounterWidth)
        || !fields::finish_exact(writer, expectedBits, expectedBytes, written)) {
        return false;
    }
    bits = expectedBits;
    return true;
}

/**
 * Encodes one retirement: root .3 disabled, an empty delivery manifest and the delivery
 * revision set to the generation. The client retires the actor on this new generation.
 * @param output Exactly kRetireBytes.
 */
[[nodiscard]] inline bool encode_retire(std::uint32_t generation,
                                        std::span<std::byte> output) noexcept {
    if (output.size() != kRetireBytes || !valid_counter(generation)) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 6> tail{{
        {0, fields::kPresenceWidth}, // .4 absent
        {0, fields::kPresenceWidth}, // .5 absent
        {0, fields::kPresenceWidth}, // .6 absent
        {1, fields::kPresenceWidth}, // .7 present
        {0, kManifestCountWidth},    // no passengers
        {generation, fields::kCounterWidth},
    }};
    return write_root(writer, generation, false) && fields::write_fields(writer, tail)
           && fields::finish_exact(writer, kRetireBits, kRetireBytes, written);
}

} // namespace sunrise::middleware::bap::activity_message::combatant_auth
