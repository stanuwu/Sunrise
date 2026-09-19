#include "character_record_encoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>

#include "../../../core/logging/log.h"
#include "appearance/internal.h"

namespace sunrise::middleware::datagen::character_record {
namespace {

/** The periodic-reset block only the family-three record carries, before the appearance. */
constexpr std::size_t kFamily3ReservedSize = layout::kPeriodicResetSize;
/** The trailing bytes after the summary block in the family-three record. */
constexpr std::size_t kFamily3TailSize = 16;
/** The trailing bytes after the summary block in the family-zero record. */
constexpr std::size_t kFamily0TailSize = 24;
/** Family-three tail offset of the card flag this record raises as local card bit 0. */
constexpr std::size_t kCardFlagOffset = 8;

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
    std::memcpy(identity.headerBlock.data(),
                layout::kHeaderBlockBytes.data(),
                layout::kHeaderBlockBytes.size());

    output = {};
    appearance::apply_sentinels(output);
    output.level = character.level;
    // The card splits this float into a whole and a partial level, so it must agree with the
    // level beside it. No progression fraction is retained, so the level itself is the value.
    output.levelProgress = static_cast<float>(character.level);
    output.primaryLight = static_cast<float>(light);
    output.auxiliaryLight = static_cast<float>(light);
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

/** Longest perk-bank report line, including the trailing entries. */
constexpr std::size_t kBankReportCapacity = 1'024;

/**
 * Reports one perk bank's occupied entries. Diagnostic only.
 * @param family Record family the bank belongs to.
 * @param bank Bank name.
 * @param entries Bank contents.
 */
void report_perk_bank(const char* family,
                      const char* bank,
                      std::span<const std::uint16_t> entries) noexcept {
    std::array<char, kBankReportCapacity> line{};
    int written = std::snprintf(
        line.data(), line.size(), "ev=appearance stage=perk_bank family=%s bank=%s", family, bank);
    if (written <= 0) {
        return;
    }
    for (const std::uint16_t entry : entries) {
        if (entry == layout::kEmptyDefinitionIndex) {
            continue;
        }
        const auto used = static_cast<std::size_t>(written);
        if (used >= line.size()) {
            break;
        }
        const int more = std::snprintf(line.data() + used, line.size() - used, " %u", entry);
        if (more <= 0) {
            break;
        }
        written += more;
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, line.data());
}

/**
 * Reports all four perk banks of one record. Diagnostic only.
 * @param family Record family the banks belong to.
 * @param block Appearance block carrying the banks.
 */
void report_perk_banks(const char* family, const layout::Appearance& block) noexcept {
    report_perk_bank(family, "index", block.indexBank);
    report_perk_bank(family, "weaponA", block.smallBankA);
    report_perk_bank(family, "weaponB", block.smallBankB);
    report_perk_bank(family, "weaponC", block.smallBankC);
}

/** @param light Equipment light. @return The trailing summary block both records carry. */
[[nodiscard]] layout::Summary build_summary(std::int32_t light,
                                            std::uint16_t titleRecordIndex) noexcept {
    layout::Summary summary{};
    summary.lightFloatA = static_cast<float>(light);
    summary.light = light;
    summary.hashA = layout::kNoHash;
    summary.indexD = titleRecordIndex;
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
    report_perk_banks("3", block);
    const auto record = output.first(kFamily3RecordSize);
    copy_record(identity,
                block,
                build_summary(light, character.equippedTitleRecordIndex),
                kFamily3ReservedSize,
                kFamily3TailSize,
                record);
    // Both stamps are the last reset before sign-in. Zero would make the client run a daily and
    // a weekly rollover as soon as it accepts the record.
    layout::PeriodicReset reset{};
    reset.lastDailyResetSeconds = character.signInSeconds;
    reset.lastWeeklyResetSeconds = character.signInSeconds;
    std::memcpy(record.data() + sizeof(layout::Identity), &reset, sizeof reset);
    // The byte after this flag is a separate inverted policy, not a copy of it: clear permits
    // the contextual card payload and set suppresses it, so it stays clear.
    const std::size_t tailStart = kFamily3RecordSize - kFamily3TailSize;
    record[tailStart + kCardFlagOffset] = character.previewAvailable ? std::byte{1} : std::byte{0};
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
    report_perk_banks("0", block);
    const auto record = output.first(kFamily0RecordSize);
    auto summary = build_summary(light, character.equippedTitleRecordIndex);
    constexpr auto emblemSlot =
        static_cast<std::uint8_t>(state::account::inventory::EquipmentSlot::emblem);
    for (std::size_t i = 0; i < instances.itemCount && i < instances.items.size(); ++i) {
        if (instances.items[i].equipmentSlot == emblemSlot) {
            summary.indexA = instances.items[i].instance.baseDefinitionIndex;
            break;
        }
    }
    copy_record(identity, block, summary, 0, kFamily0TailSize, record);
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
