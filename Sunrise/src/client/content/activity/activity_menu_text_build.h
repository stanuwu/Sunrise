#pragma once
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808031.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80805b.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80805c.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80805f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809a.h"
#include "../../../middleware/content/packages/tables/activity_presentation_table.h"

namespace sunrise::client::content::activity {
/**
 * Reads one title reference: a bank ordinal, then its string hash.
 * @tparam Bank Type of the view's bank member; 0xFFFF names no bank.
 * @param blob Blob holding the reference.
 * @param bankAt Offset of the bank ordinal.
 * @param hashAt Offset of the string hash.
 * @param output Receives the reference; unchanged on failure.
 * @return False when either field lies outside the blob.
 */
template <class Bank>
[[nodiscard]] inline bool
read_title(std::span<const std::byte> blob,
           std::size_t bankAt,
           std::size_t hashAt,
           middleware::content::packages::tables::activities::StringRef& output) noexcept {
    namespace tables = middleware::content::packages::tables;
    Bank bank{};
    std::uint32_t hash{};
    if (!tables::read(blob, bankAt, bank) || !tables::read(blob, hashAt, hash)) {
        return false;
    }
    output = {bank, hash};
    return true;
}

/** Runtime-only menu labels. Constants below are native localization identities, never text copies.
 */
inline void build_menu_text(
    const middleware::content::packages::reader::Source& source,
    middleware::content::packages::reader::Scratch& scratch,
    std::span<const std::byte> globals,
    std::span<const middleware::content::packages::tables::activities::DisplayRefs> activityRefs,
    state::build_data::activities::MenuText& output) noexcept {
    namespace packages = middleware::content::packages;
    namespace catalog = state::build_data::activities;
    namespace tables = packages::tables;
    namespace parser = tables::activities;
    namespace generated = packages::schema::generated;
    output = {};
    std::vector<std::byte> banks, container, language, bytes;
    tables::Array bankRows{};
    std::uint32_t tag{}, cls{};
    using Globals = generated::InvestmentGlobals;
    if (!tables::read(globals, offsetof(Globals, stringBankIndex), tag)
        || !packages::reader::read_tag(source, scratch, tag, banks, cls)
        || cls != generated::StringBankIndex::kClass
        || !tables::find_array_at(banks, offsetof(generated::StringBankIndex, table), bankRows)
        || bankRows.count > 8192 || bankRows.dataOffset > banks.size()
        || bankRows.count
               > (banks.size() - bankRows.dataOffset) / sizeof(generated::StringBankRow)) {
        return;
    }
    const auto resolve = [&](parser::StringRef ref, catalog::NativeText& target) noexcept {
        target.bank = ref.bank;
        target.hash = ref.hash;
        std::uint32_t root{}, english{}, actual{};
        if (ref.bank >= bankRows.count
            || !parser::read(banks,
                             bankRows.dataOffset + ref.bank * sizeof(generated::StringBankRow)
                                 + offsetof(generated::StringBankRow, container),
                             root)
            || !packages::reader::read_tag(source, scratch, root, container, actual)
            || actual != generated::SLocalizedStrings::kClass
            || !parser::read(
                container, offsetof(generated::SLocalizedStrings, languageData), english)
            || !packages::reader::read_tag(source, scratch, english, language, actual)
            || actual != generated::SLocalizedStringsData::kClass) {
            return;
        }
        (void)parser::localized_string(container, language, ref.hash, target.text);
    };
    // Installed campaign-selection localization bank.
    constexpr std::array<std::uint32_t, 5> campaignHashes{
        0x269A3283, 0x5E6C45BA, 0x99807D0D, 0xA5E7229E, 0xD07640BC};
    for (std::size_t i = 0; i < campaignHashes.size(); ++i) {
        resolve({2417, campaignHashes[i]}, output.content[i + 1]);
    }
    if (activityRefs.size() > 1) {
        resolve(activityRefs[1].title, output.content[6]);
    }
    // Seasons: the authored season row supplies its own bank/hash; no English matching or aliasing.
    tables::Array seasons{};
    if (tables::read(globals, offsetof(Globals, seasonTable), tag)
        && packages::reader::read_tag(source, scratch, tag, bytes, cls)
        && cls == generated::SeasonTable::kClass
        && tables::find_array_at(bytes, offsetof(generated::SeasonTable, table), seasons)
        && seasons.count <= 64 && seasons.dataOffset <= bytes.size()
        && seasons.count <= (bytes.size() - seasons.dataOffset) / sizeof(generated::SeasonRow)) {
        // Season rows must match these identities before their labels are used.
        constexpr std::array<std::uint32_t, 7> identities{
            0x854AC306, 0xAC5281E8, 0xFEDABB80, 0xD758957D, 0x77A58C71, 0xF088B659, 0x0ED0ED8B};
        for (std::size_t i = 0; i < identities.size(); ++i) {
            const auto row = i + 4;
            const auto offset = seasons.dataOffset + row * sizeof(generated::SeasonRow);
            std::uint32_t identity{};
            parser::StringRef ref{};
            if (row < seasons.count
                && parser::read(
                    bytes, offset + offsetof(generated::SeasonRow, seasonHash), identity)
                && identity == identities[i]
                && read_title<decltype(generated::SeasonRow::titleBank)>(
                    bytes,
                    offset + offsetof(generated::SeasonRow, titleBank),
                    offset + offsetof(generated::SeasonRow, titleHash),
                    ref)) {
                resolve(ref, output.content[i + 7]);
            }
        }
    }
    using Item = generated::ItemDisplayStrings;
    // These authored item rows supply the event title references.
    constexpr std::array<std::pair<std::size_t, std::uint32_t>, 3> itemEvents{
        {{14, 0x8161353B}, {15, 0x816135B4}, {19, 0x816134AB}}};
    for (const auto [content, item] : itemEvents) {
        parser::StringRef ref{};
        if (packages::reader::read_tag(source, scratch, item, bytes, cls) && cls == Item::kClass
            && read_title<decltype(Item::displayProperties.nameBank)>(
                bytes,
                offsetof(Item, displayProperties.nameBank),
                offsetof(Item, displayProperties.nameHash),
                ref)) {
            resolve(ref, output.content[content]);
        }
    }
    using NodeRow = generated::PresentationNodeDisplayRow;
    // One authored node row supplies the last event title; the row holds only for this build.
    constexpr std::uint32_t kNodeTable = 0x81613CF8;
    constexpr std::size_t kNodeTitleRow = 855;
    const std::size_t nodeRow = kNodeTitleRow * sizeof(NodeRow);
    tables::Array nodes{};
    parser::StringRef nodeTitle{};
    if (packages::reader::read_tag(source, scratch, kNodeTable, bytes, cls)
        && cls == generated::PresentationNodeDisplayTable::kClass
        && tables::find_array_at(
            bytes, offsetof(generated::PresentationNodeDisplayTable, table), nodes)
        && kNodeTitleRow < nodes.count && nodes.dataOffset <= bytes.size()
        && nodes.count <= (bytes.size() - nodes.dataOffset) / sizeof(NodeRow)
        && read_title<decltype(NodeRow::nameBank)>(
            bytes,
            nodes.dataOffset + nodeRow + offsetof(NodeRow, nameBank),
            nodes.dataOffset + nodeRow + offsetof(NodeRow, nameHash),
            nodeTitle)) {
        resolve(nodeTitle, output.content[20]);
    }
    resolve({2095, 0xC019C7A5},
            output.content[16]); // Installed event title, distinct from its intro quest title.
    output.content[17] =
        output.content[14]; // Same native recurring-event label; classification IDs stay distinct.
    if (activityRefs.size() > 509) {
        resolve(activityRefs[509].type, output.content[21]);
    }
    if (activityRefs.size() > 299) {
        resolve(activityRefs[299].type, output.kinds[1]);
        resolve(activityRefs[229].type, output.kinds[2]);
    }
    // Director style string containers carry the native type label under one shared key.
    constexpr std::array<std::pair<std::size_t, std::uint32_t>, 4> styles{
        {{3, 0x8161305A}, {4, 0x816131E5}, {5, 0x8161311D}, {6, 0x816132B5}}};
    for (const auto [kind, style] : styles) {
        std::uint32_t root{}, english{};
        if (!packages::reader::read_tag(source, scratch, style, bytes, cls)
            || cls != generated::DirectorStyle::kClass
            || !parser::read(bytes, offsetof(generated::DirectorStyle, stringsRoot), root)
            || !packages::reader::read_tag(source, scratch, root, container, cls)
            || cls != generated::SLocalizedStrings::kClass
            || !parser::read(
                container, offsetof(generated::SLocalizedStrings, languageData), english)
            || !packages::reader::read_tag(source, scratch, english, language, cls)
            || cls != generated::SLocalizedStringsData::kClass) {
            continue;
        }
        output.kinds[kind].bank = root;
        output.kinds[kind].hash = 0x8648376F;
        (void)parser::localized_string(container, language, 0x8648376F, output.kinds[kind].text);
    }
}
} // namespace sunrise::client::content::activity
