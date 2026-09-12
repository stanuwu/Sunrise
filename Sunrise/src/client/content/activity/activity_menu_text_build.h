#pragma once
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/activity_presentation_table.h"

namespace sunrise::client::content::activity {
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
    output = {};
    std::vector<std::byte> banks, container, language, bytes;
    tables::Array bankRows{};
    std::uint32_t tag{}, cls{};
    if (!tables::child_tag(globals, 72, tag)
        || !packages::reader::read_tag(source, scratch, tag, banks, cls) || cls != 0x80805F98
        || !tables::find_array_at(banks, 8, bankRows) || bankRows.count > 8192
        || bankRows.dataOffset > banks.size()
        || bankRows.count > (banks.size() - bankRows.dataOffset) / 8) {
        return;
    }
    const auto resolve = [&](parser::StringRef ref, catalog::NativeText& target) noexcept {
        target.bank = ref.bank;
        target.hash = ref.hash;
        std::uint32_t root{}, english{}, actual{};
        if (ref.bank >= bankRows.count
            || !parser::read(banks, bankRows.dataOffset + ref.bank * 8 + 4, root)
            || !packages::reader::read_tag(source, scratch, root, container, actual)
            || actual != 0x80809A88 || !parser::read(container, 24, english)
            || !packages::reader::read_tag(source, scratch, english, language, actual)
            || actual != 0x80809A8A) {
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
    if (tables::child_tag(globals, 54, tag)
        && packages::reader::read_tag(source, scratch, tag, bytes, cls) && cls == 0x8080311B
        && tables::find_array_at(bytes, 8, seasons) && seasons.count <= 64
        && seasons.dataOffset <= bytes.size()
        && seasons.count <= (bytes.size() - seasons.dataOffset) / 72) {
        constexpr std::array<std::uint32_t, 7> identities{
            0x854AC306, 0xAC5281E8, 0xFEDABB80, 0xD758957D, 0x77A58C71, 0xF088B659, 0x0ED0ED8B};
        for (std::size_t i = 0; i < identities.size(); ++i) {
            const auto row = i + 4;
            std::uint32_t identity{};
            parser::StringRef ref{};
            if (row < seasons.count
                && parser::read(bytes, seasons.dataOffset + row * 72 + 8, identity)
                && identity == identities[i]
                && parser::read(bytes, seasons.dataOffset + row * 72 + 36, ref)) {
                resolve(ref, output.content[i + 7]);
            }
        }
    }
    struct Event {
        std::size_t content;
        std::uint32_t tag, cls;
        std::size_t offset;
    };
    constexpr std::array<Event, 4> events{{{14, 0x8161353B, 0x80805CE1, 132},
                                           {15, 0x816135B4, 0x80805CE1, 132},
                                           {19, 0x816134AB, 0x80805CE1, 132},
                                           {20, 0x81613CF8, 0x80802E11, 37676}}};
    for (const auto& event : events) {
        parser::StringRef ref{};
        if (packages::reader::read_tag(source, scratch, event.tag, bytes, cls) && cls == event.cls
            && parser::read(bytes, event.offset, ref)) {
            resolve(ref, output.content[event.content]);
        }
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
        if (!packages::reader::read_tag(source, scratch, style, bytes, cls) || cls != 0x80802A38
            || !parser::read(bytes, 16, root)
            || !packages::reader::read_tag(source, scratch, root, container, cls)
            || cls != 0x80809A88 || !parser::read(container, 24, english)
            || !packages::reader::read_tag(source, scratch, english, language, cls)
            || cls != 0x80809A8A) {
            continue;
        }
        output.kinds[kind].bank = root;
        output.kinds[kind].hash = 0x8648376F;
        (void)parser::localized_string(container, language, 0x8648376F, output.kinds[kind].text);
    }
}
} // namespace sunrise::client::content::activity
