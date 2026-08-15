#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <utility>

#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::items::details;

/** Equipment slot for each equippable inventory bucket. */
constexpr std::array<std::pair<std::uint8_t, std::int8_t>, 16> kEquipmentSlotOfBucket{{
    {16, 0},
    {3, 1},
    {4, 2},
    {5, 4},
    {6, 5},
    {7, 6},
    {0, 7},
    {1, 8},
    {2, 9},
    {10, 10},
    {9, 11},
    {8, 12},
    {27, 13},
    {41, 14},
    {17, 15},
    {47, 17},
}};

/** @param bucketId Inventory bucket. @return Its equipment slot, or none when not equippable. */
[[nodiscard]] std::optional<std::int8_t> equipment_slot(std::uint8_t bucketId) noexcept {
    for (const auto& entry : kEquipmentSlotOfBucket) {
        if (entry.first == bucketId) {
            return entry.second;
        }
    }
    return std::nullopt;
}

/** @param row Package row. @return Its cached detail form. */
[[nodiscard]] domain::Definition to_detail(const tables::items::Row& row) noexcept {
    domain::Definition detail{};
    detail.definitionIndex = row.definitionIndex;
    detail.definitionHash = row.definitionHash;
    detail.bucketId = row.bucketId;
    detail.maxStackSize = row.maxStackSize;
    detail.instancedDefinitionState = row.instanced ? domain::InstancedDefinitionState::instanced
                                                    : domain::InstancedDefinitionState::stackable;
    detail.equipmentSlot = equipment_slot(row.bucketId);
    detail.ordinarySocketState =
        row.hasSockets ? domain::OrdinarySocketState::present : domain::OrdinarySocketState::absent;
    detail.ordinarySocketCount = row.socketCount;
    for (std::size_t lane = 0; lane < detail.initialPlugIndices.size(); ++lane) {
        detail.initialPlugIndices[lane] = row.initialPlugs[lane];
        detail.socketTypes[lane] = row.socketTypes[lane];
    }
    detail.socketEntryListIndex = row.socketEntryListIndex;
    // Every field below comes from the package blob only; the loaded definition is never read.
    const std::size_t stats =
        row.statCount < detail.stats.size() ? row.statCount : detail.stats.size();
    for (std::size_t entry = 0; entry < stats; ++entry) {
        detail.stats[entry] = {row.statRows[entry], row.statValues[entry]};
    }
    detail.statCount = static_cast<std::uint8_t>(stats);
    detail.gearArtIndex = row.gearArtIndex;
    detail.artArrangementIndex = row.artArrangementIndex;
    const std::size_t perks = row.sandboxPerkCount < detail.sandboxPerks.size()
                                  ? row.sandboxPerkCount
                                  : detail.sandboxPerks.size();
    for (std::size_t entry = 0; entry < perks; ++entry) {
        detail.sandboxPerks[entry] = row.sandboxPerks[entry];
    }
    detail.sandboxPerkCount = static_cast<std::uint8_t>(perks);
    const std::size_t overrides = row.renderOverrideCount < detail.renderOverrides.size()
                                      ? row.renderOverrideCount
                                      : detail.renderOverrides.size();
    for (std::size_t entry = 0; entry < overrides; ++entry) {
        detail.renderOverrides[entry] = {row.renderOverrides[entry].stage,
                                         row.renderOverrides[entry].key,
                                         row.renderOverrides[entry].value};
    }
    detail.renderOverrideCount = static_cast<std::uint8_t>(overrides);
    return detail;
}

/** The constants blob's own 8-byte prefix comes before every offset the client quotes. */
constexpr std::size_t kConstantsPrefix = 8;
/** Client offset of the stat row the banner's power number is searched by. */
constexpr std::size_t kLightStatRowOffset = 592;
/**
 * Client offsets of the 6 character stat rows, in the two runs the blob stores them in.
 * The client reads these as 6 separate scalars, not as one array, so each is named here.
 */
constexpr std::size_t kCharacterStatRowOffsets[]{593, 594, 595, 622, 623, 624};

} // namespace

/** Collects the authored equipment and plug hashes every configured character names. */
bool collect_authored_hashes(AuthoredHashes& output) noexcept {
    output = {};
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account)) {
        return false;
    }
    for (std::size_t character = 0; character < account.characterCount; ++character) {
        for (const auto& item : account.characters[character].equipment.slots) {
            if (!item.has_value() || output.count >= output.values.size()) {
                continue;
            }
            output.values[output.count++] = item->definitionHash;
            for (std::size_t lane = 0; lane < item->sockets.plugCount; ++lane) {
                if (item->sockets.plugs[lane].has_value() && output.count < output.values.size()) {
                    output.values[output.count++] = *item->sockets.plugs[lane];
                }
            }
        }
    }
    const auto end = output.values.begin() + static_cast<std::ptrdiff_t>(output.count);
    std::sort(output.values.begin(), end);
    output.count =
        static_cast<std::size_t>(std::unique(output.values.begin(), end) - output.values.begin());
    return output.count != 0;
}

/** @param hashes Sorted authored hashes. @param hash Row hash. @return True when authored. */
bool authored(const AuthoredHashes& hashes, std::uint32_t hash) noexcept {
    const auto begin = hashes.values.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(hashes.count);
    return std::binary_search(begin, end, hash);
}

/** Adds one definition index to the requested set. */
bool request(std::uint16_t definitionIndex,
             std::span<std::uint16_t> requested,
             std::size_t& count) noexcept {
    if (count >= requested.size()) {
        return false;
    }
    requested[count++] = definitionIndex;
    return true;
}

/** Adds every socket lane's initial plug to the requested set. */
bool append_initial_plugs(const tables::items::Row& row,
                          std::span<std::uint16_t> requested,
                          std::size_t& count) noexcept {
    for (std::size_t lane = 0; lane < row.socketCount; ++lane) {
        if (row.initialPlugs[lane] == tables::items::kUnavailablePlug) {
            continue;
        }
        if (!request(row.initialPlugs[lane], requested, count)) {
            return false;
        }
    }
    return true;
}

/** @param requested Requested-set storage. @param count Sorted and deduplicated in place. */
void compact_requested(std::span<std::uint16_t> requested, std::size_t& count) noexcept {
    auto end = requested.begin() + static_cast<std::ptrdiff_t>(count);
    std::sort(requested.begin(), end);
    end = std::unique(requested.begin(), end);
    count = static_cast<std::size_t>(end - requested.begin());
}

/** Reads one requested definition and turns it into its cached detail form. */
bool build_detail(const DetailSource& source,
                  std::uint16_t definitionIndex,
                  domain::Definition& detail) noexcept {
    tables::IndexRow indexRow{};
    tables::items::Row item{};
    item.definitionIndex = definitionIndex;
    if (!tables::index_row(source.table, source.array, definitionIndex, indexRow)
        || !reader::read_tag(
            *source.source, *source.scratch, indexRow.targetTag, *source.definition)
        || !tables::items::read_definition(std::span<const std::byte>{*source.definition}, item)) {
        return false;
    }
    item.definitionHash = indexRow.definitionHash;
    detail = to_detail(item);
    return true;
}

/** Reads the stat rows the installed investment constants blob names. */
bool read_investment_constants(const reader::Source& source,
                               reader::Scratch& scratch,
                               std::span<const std::byte> root,
                               std::vector<std::byte>& blob,
                               state::build_data::constants::InvestmentConstants& output) noexcept {
    output = {};
    std::uint32_t tag = 0;
    if (!tables::slot_tag(root, tables::kInvestmentConstantsSlot, tag) || tag == 0
        || !reader::read_tag(source, scratch, tag, blob)) {
        return false;
    }
    const std::size_t last =
        kConstantsPrefix + kCharacterStatRowOffsets[std::size(kCharacterStatRowOffsets) - 1];
    if (blob.size() <= last) {
        return false;
    }
    output.lightStatRow =
        std::to_integer<std::uint8_t>(blob[kConstantsPrefix + kLightStatRowOffset]);
    for (std::size_t row = 0; row < std::size(kCharacterStatRowOffsets); ++row) {
        output.characterStatRows[row] =
            std::to_integer<std::uint8_t>(blob[kConstantsPrefix + kCharacterStatRowOffsets[row]]);
    }
    output.extracted = true;
    return true;
}

} // namespace sunrise::client::content::items::packages
