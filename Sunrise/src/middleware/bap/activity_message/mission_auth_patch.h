#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"
#include "squad_auth_body.h"

// A native sensor override replaces the whole Auth object. A mission API body carries only the
// root fields it sets, so it is composed over the last transported body before it is staged.
// A field without a presence bit cannot be left out, so a patch also names the fields it sets.
// TODO: the field tables below repeat the squad and combatant layouts their codecs own. Read the
// layout from the codecs once they expose it.

namespace sunrise::middleware::bap::activity_message::mission_auth_patch {

namespace fields = auth_fields;

/** Largest body either layout produces. */
inline constexpr std::size_t kCapacity = 256;
/** Root field counts of the two supported layouts. */
inline constexpr std::size_t kSquadFieldCount = 21;
inline constexpr std::size_t kCombatantFieldCount = 8;

/** Root fields one patch sets, one bit per field ordinal. */
using FieldSet = std::uint32_t;
static_assert((std::max)(kSquadFieldCount, kCombatantFieldCount)
              <= std::numeric_limits<FieldSet>::digits);
/** A patch that sets every root field it carries. */
inline constexpr FieldSet kAllFields = (std::numeric_limits<FieldSet>::max)();

/** @return The set holding only root field `index`. */
[[nodiscard]] constexpr FieldSet field_bit(std::size_t index) noexcept {
    return FieldSet{1} << index;
}

/** How one root field's width is found. */
enum class Kind : std::uint8_t {
    /** Fixed width. */
    fixed,
    /** A 4-bit count of 32-bit values. */
    counted32,
    /** Squad field .2: a 3-bit count of 32-bit temperament ids. */
    temperaments,
    /** A field this composer does not carry; a present one refuses the body. */
    refused,
    /** Combatant field .6: the program header, then a kind-specific body. */
    program,
    /** Combatant field .7: a 4-bit squad count, one ClientRef each, then a 31-bit revision. */
    manifest,
};

/** One root field: whether a presence bit precedes it, and how wide it is. */
struct FieldRule final {
    Kind kind{Kind::fixed};
    bool optional{true};
    std::uint16_t width{};
};

/** Squad Auth root fields .0 to .20. Widths are bits after the presence bit. */
inline constexpr std::array<FieldRule, kSquadFieldCount> kSquadRules{{
    {Kind::fixed, true, fields::kClientRefBits}, // .0 objective ClientRef
    {Kind::fixed, true, fields::kClientRefBits}, // .1
    {Kind::temperaments, true, 0},               // .2 temperaments
    {Kind::counted32, true, 0},                  // .3 requested counts
    {Kind::counted32, true, 0},                  // .4
    {Kind::fixed, true, 13},                     // .5 authored profile
    {Kind::fixed, true, 31},                     // .6 spawn generation
    {Kind::fixed, true, 32},                     // .7
    {Kind::fixed, true, 32},                     // .8
    {Kind::fixed, true, fields::kClientRefBits}, // .9
    {Kind::fixed, true, fields::kClientRefBits}, // .10
    {Kind::fixed, true, fields::kClientRefBits}, // .11 spawn reference
    {Kind::fixed, true, fields::kClientRefBits}, // .12 spawn reference
    {Kind::fixed, true, 31},                     // .13 objective revision
    {Kind::fixed, true, 31},                     // .14
    {Kind::fixed, true, 6},                      // .15
    {Kind::fixed, true, 5},                      // .16 task group
    {Kind::fixed, true, 31},                     // .17
    {Kind::fixed, false, 2},                     // .18 active
    {Kind::fixed, false, 3},                     // .19 mode
    {Kind::fixed, true, 32},                     // .20 name hash
}};

/** Combatant Auth root fields .0 to .7. */
inline constexpr std::array<FieldRule, kCombatantFieldCount> kCombatantRules{{
    {Kind::fixed, true, 31},   // .0 spawn generation
    {Kind::fixed, false, 2},   // .1 mode
    {Kind::fixed, false, 3},   // .2 marker
    {Kind::fixed, false, 1},   // .3 enabled
    {Kind::refused, true, 0},  // .4
    {Kind::refused, true, 0},  // .5
    {Kind::program, true, 0},  // .6
    {Kind::manifest, true, 0}, // .7
}};

/** Combatant .6 program layout: revision, two 6-bit header words, presence, kind, completion. */
inline constexpr std::size_t kProgramLeadBits = 31 + 6;
inline constexpr std::uint8_t kProgramHeaderWidth = 6;
inline constexpr std::uint32_t kProgramHeaderSecond = 1;
inline constexpr std::uint8_t kProgramKindWidth = 4;
inline constexpr std::uint8_t kCompletionWidth = 2;
inline constexpr std::uint32_t kCompletionValue = 1;
/** Biased program kinds this composer carries: a path (3) and an action (9). */
inline constexpr std::uint32_t kPathKindWire = 4;
inline constexpr std::uint32_t kActionKindWire = 10;
/** Bits after the completion selector: a path ClientRef, marker and flag; or the action body. */
inline constexpr std::size_t kPathBodyBits = fields::kClientRefBits + 8 + 1;
inline constexpr std::size_t kActionBodyBits = 32 + 32 + 32 + fields::kClientRefBits + 3 + 8;
/** Combatant .7 manifest: a 4-bit count of ClientRefs, then a 31-bit delivery revision. */
inline constexpr std::uint8_t kCountWidth = 4;
inline constexpr std::size_t kMaximumCount = 8;

/** Where one root field sits in a parsed body. */
struct Field final {
    std::size_t offset{};
    std::size_t bits{};
    bool present{};
};

struct Layout final {
    std::array<Field, kSquadFieldCount> fields{};
    std::size_t count{};
};

/** @return The rules for one supported schema, or an empty span. */
[[nodiscard]] inline std::span<const FieldRule> rules_for(std::uint32_t schema) noexcept {
    if (schema == squad_auth::kSchema) {
        return kSquadRules;
    }
    if (schema == scriptable_auth::kType2Schema) {
        return kCombatantRules;
    }
    return {};
}

/** Skips a combatant .6 program body. @return False on a kind this composer does not carry. */
[[nodiscard]] inline bool skip_program(encoding::bits::Reader& reader) noexcept {
    std::uint64_t value = 0;
    if (!reader.skip(kProgramLeadBits) || !reader.read(kProgramHeaderWidth, value)
        || value != kProgramHeaderSecond || !reader.read(fields::kPresenceWidth, value)
        || value != 1 || !reader.read(kProgramKindWidth, value)
        || (value != kPathKindWire && value != kActionKindWire)) {
        return false;
    }
    const std::uint64_t kind = value;
    return reader.read(kCompletionWidth, value) && value == kCompletionValue
           && reader.skip(kind == kPathKindWire ? kPathBodyBits : kActionBodyBits);
}

/** Skips a combatant .7 manifest. */
[[nodiscard]] inline bool skip_manifest(encoding::bits::Reader& reader) noexcept {
    std::uint64_t count = 0;
    return reader.read(kCountWidth, count) && count <= kMaximumCount
           && reader.skip(fields::kClientRefBits * count + fields::kCounterWidth);
}

/**
 * Locates every root field of one body.
 * @param bits Meaningful bit count of the body.
 * @return False on an unsupported schema, a refused field, a size mismatch or nonzero padding.
 */
[[nodiscard]] inline bool parse(std::uint32_t schema,
                                std::span<const std::byte> body,
                                std::size_t bits,
                                Layout& out) noexcept {
    const std::span<const FieldRule> rules = rules_for(schema);
    if (rules.empty() || body.empty() || body.size() > kCapacity
        || !fields::bytes_match_bits(body.size(), bits)) {
        return false;
    }
    encoding::bits::Reader reader(body);
    const auto position = [&] { return body.size() * 8 - reader.remaining_bits(); };
    out = {};
    out.count = rules.size();
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const FieldRule& rule = rules[index];
        Field& field = out.fields[index];
        field.offset = position();
        std::uint64_t present = 1;
        if (rule.optional && !reader.read(fields::kPresenceWidth, present)) {
            return false;
        }
        field.present = present != 0;
        if (field.present) {
            std::uint64_t count = 0;
            bool skipped = false;
            switch (rule.kind) {
            case Kind::fixed:
                skipped = reader.skip(rule.width);
                break;
            case Kind::counted32:
                skipped = reader.read(kCountWidth, count) && count <= kMaximumCount
                          && reader.skip(32 * count);
                break;
            case Kind::temperaments:
                skipped =
                    reader.read(static_cast<std::uint8_t>(squad_auth::kTemperamentCountBitCount),
                                count)
                    && count <= squad_auth::kTemperamentCapacity
                    && reader.skip(squad_auth::kTemperamentIdBitCount * count);
                break;
            case Kind::program:
                skipped = skip_program(reader);
                break;
            case Kind::manifest:
                skipped = skip_manifest(reader);
                break;
            case Kind::refused:
                break;
            }
            if (!skipped) {
                return false;
            }
        }
        field.bits = position() - field.offset;
    }
    std::uint64_t padding = 0;
    return position() == bits
           && reader.read(static_cast<std::uint8_t>(body.size() * 8 - bits), padding)
           && padding == 0;
}

/** Copies one located field, in 64-bit pieces, from a body into the writer. */
[[nodiscard]] inline bool
copy_field(encoding::bits::Writer& writer, std::span<const std::byte> body, Field field) noexcept {
    encoding::bits::Reader reader(body);
    if (!reader.skip(field.offset)) {
        return false;
    }
    while (field.bits != 0) {
        const auto piece = static_cast<std::uint8_t>(field.bits > 64 ? 64 : field.bits);
        std::uint64_t value = 0;
        if (!reader.read(piece, value) || !writer.write(value, piece)) {
            return false;
        }
        field.bits -= piece;
    }
    return true;
}

/**
 * Composes a patch over the previous body: a field outside `patchFields`, or one the patch leaves
 * absent, keeps the previous value. With no previous body the patch is written as it is.
 * @param written Receives the byte count. @param bits Receives the meaningful bit count.
 * @return False when either body fails parse() or the result does not fit output.
 */
[[nodiscard]] inline bool compose(std::uint32_t schema,
                                  std::span<const std::byte> previous,
                                  std::size_t previousBits,
                                  std::span<const std::byte> patch,
                                  std::size_t patchBits,
                                  FieldSet patchFields,
                                  std::span<std::byte> output,
                                  std::size_t& written,
                                  std::size_t& bits) noexcept {
    written = 0;
    bits = 0;
    Layout old{};
    Layout next{};
    if (!parse(schema, patch, patchBits, next)
        || (!previous.empty() && !parse(schema, previous, previousBits, old))) {
        return false;
    }
    std::array<std::byte, kCapacity> staged{};
    encoding::bits::Writer writer(staged);
    for (std::size_t index = 0; index < next.count; ++index) {
        const bool patchSets = (patchFields & field_bit(index)) != 0;
        const bool keep =
            !previous.empty()
            && (!patchSets || (!next.fields[index].present && old.fields[index].present));
        if (!copy_field(
                writer, keep ? previous : patch, keep ? old.fields[index] : next.fields[index])) {
            return false;
        }
    }
    const std::size_t usedBits = writer.bit_count();
    std::size_t used = 0;
    if (!writer.finish(used) || output.size() < used) {
        return false;
    }
    for (std::size_t index = 0; index < used; ++index) {
        output[index] = staged[index];
    }
    written = used;
    bits = usedBits;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::mission_auth_patch
