#include "character_record_encoder.h"

#include <algorithm>
#include <cstring>

#include "appearance/internal.h"

namespace sunrise::middleware::datagen::character_record {
namespace {

/** The reserved block only the family-three record carries, between identity and appearance. */
constexpr std::size_t kFamily3ReservedSize = 56;
/** The trailing bytes after the summary block in the family-three record. */
constexpr std::size_t kFamily3TailSize = 16;
/** The trailing bytes after the summary block in the family-zero record. */
constexpr std::size_t kFamily0TailSize = 24;
/** Offsets inside the family-three tail that carry the character-select preview flags. */
constexpr std::size_t kPreviewFlagOffsets[]{8, 9};

/**
 * Builds the identity prefix and appearance block both records share byte for byte.
 * @param character Validated authored character.
 * @param instances Resolved item instances belonging to that character.
 * @param light Equipment light the banner displays.
 * @param identity Receives the identity prefix.
 * @param output Receives the appearance block.
 * @return True when every instance addresses a render row and the stat rows are known.
 */
[[nodiscard]] bool build_shared(const state::CharacterState& character,
                                const family4::loadout::ResolvedInstances& instances,
                                std::int32_t light,
                                layout::Identity& identity,
                                layout::Appearance& output) noexcept {
    identity = {};
    identity.characterSoid = character.soid;
    identity.identity = {
        static_cast<std::int8_t>(character.race),
        static_cast<std::int8_t>(character.gender),
        static_cast<std::int8_t>(character.characterClass),
    };
    const bool hasNativePresentation = std::any_of(character.presentationHeader.begin(),
                                                   character.presentationHeader.end(),
                                                   [](std::byte value) {
                                                       return value != std::byte{};
                                                   });
    if (hasNativePresentation) {
        std::memcpy(identity.headerBlock.data(),
                    character.presentationHeader.data(),
                    character.presentationHeader.size());
    } else {
        std::memcpy(identity.headerBlock.data(),
                    layout::kHeaderBlockBytes.data(),
                    layout::kHeaderBlockBytes.size());
    }

    output = {};
    appearance::apply_sentinels(output);
    output.level = character.level;
    output.unusedFloatA = 1.0F;
    output.unusedFloatB = static_cast<float>(light);
    output.light = static_cast<float>(light);
    if (!appearance::apply_render(instances, character.characterClass, output)
        || !appearance::apply_stats(instances, light, output)
        || !appearance::apply_ability_buckets(character, instances, output)) {
        return false;
    }
    // The ability buckets claim their overflow slots first; the gear hashes take what is left.
    appearance::apply_overflow_hashes(instances, output);
    appearance::apply_perk_banks(instances, output);
    return true;
}

/** @param light Equipment light. @return The trailing summary block both records carry. */
[[nodiscard]] layout::Summary build_summary(std::int32_t light) noexcept {
    layout::Summary summary{};
    summary.light = light;
    summary.hashA = layout::kNoHash;
    return summary;
}

/**
 * Copies the identity, appearance and summary into one record with its own reserved spans.
 * @param identity Shared identity prefix.
 * @param block Shared appearance block.
 * @param summary Shared summary block.
 * @param reservedSize Reserved bytes between identity and appearance.
 * @param tailSize Reserved bytes after the summary.
 * @param output Exact record storage.
 */
void copy_record(const layout::Identity& identity,
                 const layout::Appearance& block,
                 const layout::Summary& summary,
                 std::size_t reservedSize,
                 std::size_t tailSize,
                 std::span<std::byte> output) noexcept {
    std::fill(output.begin(), output.end(), std::byte{});
    std::size_t cursor = 0;
    std::memcpy(output.data() + cursor, &identity, sizeof identity);
    cursor += sizeof(identity) + reservedSize;
    std::memcpy(output.data() + cursor, &block, sizeof block);
    cursor += sizeof block;
    std::memcpy(output.data() + cursor, &summary, sizeof summary);
    cursor += sizeof(summary) + tailSize;
    (void)cursor;
}

} // namespace

/** Encodes one family-three character record. */
bool encode_family3(const state::CharacterState& character,
                    const family4::loadout::ResolvedInstances& instances,
                    std::int32_t light,
                    std::span<std::byte> output) noexcept {
    layout::Identity identity{};
    layout::Appearance block{};
    if (output.size() < kFamily3RecordSize
        || !build_shared(character, instances, light, identity, block)) {
        return false;
    }
    const auto record = output.first(kFamily3RecordSize);
    copy_record(
        identity, block, build_summary(light), kFamily3ReservedSize, kFamily3TailSize, record);
    // The character-select preview flags sit past the summary; their accessor returns true when
    // the context is missing, so a cleared flag asserts the opposite of the client's own fallback.
    const std::size_t tailStart = kFamily3RecordSize - kFamily3TailSize;
    for (const std::size_t offset : kPreviewFlagOffsets) {
        record[tailStart + offset] = character.previewAvailable ? std::byte{1} : std::byte{0};
    }
    return true;
}

/** Encodes one family-zero banner record. */
bool encode_family0(const state::CharacterState& character,
                    const family4::loadout::ResolvedInstances& instances,
                    std::int32_t light,
                    std::span<std::byte> output) noexcept {
    layout::Identity identity{};
    layout::Appearance block{};
    if (output.size() < kFamily0RecordSize
        || !build_shared(character, instances, light, identity, block)) {
        return false;
    }
    const auto record = output.first(kFamily0RecordSize);
    copy_record(identity, block, build_summary(light), 0, kFamily0TailSize, record);
    const layout::Family0Tail tail{};
    std::memcpy(record.data() + kFamily0RecordSize - kFamily0TailSize, &tail, sizeof tail);
    return true;
}

/** Encodes the family-zero banner anchor. */
bool encode_family0_anchor(std::uint64_t accountSoid,
                           std::uint64_t characterSoid,
                           std::span<std::byte> output) noexcept {
    if (output.size() < kFamily0AnchorSize) {
        return false;
    }
    const auto anchor = output.first(kFamily0AnchorSize);
    std::fill(anchor.begin(), anchor.end(), std::byte{});
    std::memcpy(anchor.data(), &accountSoid, sizeof accountSoid);
    std::memcpy(anchor.data() + sizeof accountSoid, &characterSoid, sizeof characterSoid);
    return true;
}

} // namespace sunrise::middleware::datagen::character_record
