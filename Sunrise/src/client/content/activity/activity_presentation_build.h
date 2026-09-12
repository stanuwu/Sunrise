#pragma once

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/activity_presentation_table.h"
#include "../../../state/build_data/activities/activity_artwork.h"
#include "../../../state/build_data/activities/activity_releases.h"
#include "activity_menu_text_build.h"

namespace sunrise::client::content::activity {
/** Native Director campaign/activity styles, verified against the installed style strings.
 * +12 image set -> variant 2 (82px colored selection shield) -> image -> RGBA texture.
 * These are artwork identities, never destination overrides or launch IDs. */
inline void build_artwork(const middleware::content::packages::reader::Source& source,
                          middleware::content::packages::reader::Scratch& scratch) noexcept {
    namespace packages = middleware::content::packages;
    namespace catalog = state::build_data::activities;
    namespace parser = packages::tables::activities;
    constexpr std::array<std::uint32_t, catalog::kIconCount> styles{
        0,          0x816130A9, 0x8161308D, 0x816130C9, 0x8161309B, 0x816130B9, 0x8161319B,
        0x8161331E, 0x816132CF, 0x8161311D, 0x816131E5, 0x81613302, 0x81613292, 0x81613133,
        0x8161305A, 0x816131CF, 0x816132F1, 0x81613175, 0x81613076, 0x816131C0, 0x8161338B,
        0x81613154, 0x81613206, 0x816131F0, 0x8161321C, 0x81613211, 0x816131FB, 0x816132B5};
    std::array<catalog::Artwork, catalog::kIconCount> images{};
    std::vector<std::byte> bytes, pixels;
    const auto read = [&](std::uint32_t tag, std::uint32_t expected) noexcept {
        std::uint32_t actual{};
        return packages::reader::read_tag(source, scratch, tag, bytes, actual)
               && actual == expected;
    };
    const auto texture = [&](std::size_t i, std::uint32_t tag) noexcept {
        std::uint32_t format{}, buffer{}, large{};
        std::uint16_t cafe{}, width{}, height{}, depth{}, layers{};
        if (!packages::reader::read_tag(source, scratch, tag, bytes, buffer) || bytes.size() != 40
            || !parser::read(bytes, 4, format) || format != 28 || !parser::read(bytes, 12, cafe)
            || cafe != 0xCAFE || !parser::read(bytes, 14, width) || !parser::read(bytes, 16, height)
            || !parser::read(bytes, 18, depth) || !parser::read(bytes, 20, layers) || width == 0
            || height == 0 || width > 256 || height > 256 || depth != 1 || layers != 1
            || !parser::read(bytes, 36, large) || large != 0xFFFFFFFFU) {
            return;
        }
        const auto size = static_cast<std::size_t>(width) * height * 4;
        if (!packages::reader::read_tag(source, scratch, buffer, pixels) || pixels.size() < size
            || pixels.size() > 512 * 1024) {
            return;
        }
        images[i].tag = tag;
        images[i].width = width;
        images[i].height = height;
        images[i].pixels.assign(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(size));
    };
    for (std::size_t i = 1; i < styles.size(); ++i) {
        if (styles[i] == 0) {
            continue;
        }
        std::uint32_t tag{};
        packages::tables::Array variants{};
        if (!read(styles[i], 0x80802A38) || !parser::read(bytes, 12, tag) || !read(tag, 0x80804A55)
            || !packages::tables::find_array_at(bytes, 8, variants) || variants.count < 3
            || variants.count > 16 || !parser::read(bytes, variants.dataOffset + 2 * 112 + 4, tag)
            || !read(tag, 0x80804A69) || !parser::read(bytes, 128, tag)) {
            continue;
        }
        texture(i, tag);
    }
    // Authored icon indices from exact season/event/intro-quest records.
    // The expected texture guards this build-specific join; no replacement artwork is synthesized.
    struct NativeIcon {
        catalog::Icon icon;
        std::uint16_t index;
        std::uint32_t expectedTexture;
        std::uint8_t field{20}, variant{};
    };
    constexpr std::array<NativeIcon, 13> eventIcons{
        {// The 32px season-table marks are smaller than the standalone 45px inventory marks.
         // Variant 3 is the complete authored glyph; 96px variants are corner tiles with a smaller
         // glyph.
         {catalog::Icon{28}, 12109, 0x813217EC, 32, 3},
         {catalog::Icon{29}, 13329, 0x81A2760C, 32, 3},
         {catalog::Icon{30}, 13610, 0x81A27409, 32, 3},
         {catalog::Icon{31}, 14737, 0x81A27487, 32, 3},
         {catalog::Icon{32}, 10586, 0x8132D4B0},
         {catalog::Icon{33}, 5973, 0x8132D6B0},
         {catalog::Icon{34}, 5964, 0x8132D573},
         {catalog::Icon{35}, 5980, 0x8132D4E6},
         {catalog::Icon{36}, 11203, 0x8132D914},
         {catalog::Icon{37}, 16, 0x81320DB2},
         {catalog::Icon{38}, 10541, 0x813185CC, 32, 3},
         {catalog::Icon{39}, 11305, 0x813217A8, 32, 3},
         {catalog::Icon{40}, 11578, 0x813217CA, 32, 3}}};
    packages::tables::Array iconRows{};
    if (read(0x81A291C2, 0x80802951) && packages::tables::find_array_at(bytes, 8, iconRows)
        && iconRows.count <= 20000 && iconRows.dataOffset <= bytes.size()
        && iconRows.count <= (bytes.size() - iconRows.dataOffset) / 24) {
        std::vector<std::byte> iconMap;
        iconMap.swap(bytes);
        for (const auto& icon : eventIcons) {
            std::uint32_t tag{};
            packages::tables::Array frames{}, textures{};
            if (icon.index >= iconRows.count
                || !parser::read(iconMap, iconRows.dataOffset + icon.index * 24 + 16, tag)
                || !read(tag, 0x80804A53) || !parser::read(bytes, icon.field, tag)
                || !read(tag, 0x80804A69) || !packages::tables::find_array_at(bytes, 32, frames)
                || frames.count != 1
                || !packages::tables::find_array_at(bytes, frames.dataOffset, textures)
                || textures.count <= icon.variant || textures.count > 16
                || textures.dataOffset > bytes.size()
                || textures.count > (bytes.size() - textures.dataOffset) / 4
                || !parser::read(bytes, textures.dataOffset + icon.variant * 4, tag)
                || tag != icon.expectedTexture) {
                continue;
            }
            texture(static_cast<std::size_t>(icon.icon), tag);
        }
    }
    (void)catalog::publish_artwork(std::move(images));
}

inline void
build_presentations(const middleware::content::packages::reader::Source& source,
                    middleware::content::packages::reader::Scratch& scratch,
                    std::span<const std::byte> globals,
                    std::span<const state::build_data::activities::Definition> rows) noexcept {
    namespace packages = middleware::content::packages;
    namespace catalog = state::build_data::activities;
    namespace tables = packages::tables;
    namespace parser = tables::activities;
    std::vector<std::byte> display, types, banks, container, language;
    const auto child =
        [&](std::size_t slot, std::uint32_t expected, std::vector<std::byte>& target) noexcept {
            std::uint32_t tag{}, actual{};
            return tables::child_tag(globals, slot, tag)
                   && packages::reader::read_tag(source, scratch, tag, target, actual)
                   && actual == expected;
        };
    static std::array<parser::DisplayRefs, catalog::kCapacity> refs{};
    static std::array<catalog::Presentation, catalog::kCapacity> presentation{};
    tables::Array bankRows{};
    if (!child(3, 0x80805E0A, display) || !child(6, 0x80805F3C, types)
        || !child(72, 0x80805F98, banks) || !tables::find_array_at(banks, 8, bankRows)
        || bankRows.count > 8192 || bankRows.dataOffset > banks.size()
        || bankRows.count > (banks.size() - bankRows.dataOffset) / 8
        || !parser::display_refs(display, types, rows, refs)) {
        return;
    }
    // Read each requested bank once; scratch block caching remains owned by the investment worker.
    for (std::uint32_t bank = 0; bank < bankRows.count; ++bank) {
        bool needed{};
        for (std::size_t i = 0; i < rows.size() && !needed; ++i) {
            needed = refs[i].title.bank == bank || refs[i].description.bank == bank
                     || refs[i].type.bank == bank || refs[i].expansion.bank == bank;
        }
        if (!needed) {
            continue;
        }
        std::uint32_t tag{}, actual{}, english{};
        if (!parser::read(banks, bankRows.dataOffset + bank * 8 + 4, tag)
            || !packages::reader::read_tag(source, scratch, tag, container, actual)
            || actual != 0x80809A88 || !parser::read(container, 24, english)
            || !packages::reader::read_tag(source, scratch, english, language, actual)
            || actual != 0x80809A8A) {
            continue;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto resolve = [&](parser::StringRef ref, std::span<char> text) noexcept {
                if (ref.bank == bank) {
                    (void)parser::localized_string(container, language, ref.hash, text);
                }
            };
            resolve(refs[i].title, presentation[i].title);
            resolve(refs[i].description, presentation[i].description);
            resolve(refs[i].type, presentation[i].type);
            resolve(refs[i].expansion, presentation[i].expansion);
        }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto& p = presentation[i];
        p.nativeStyle = refs[i].style;
        p.experience = rows[i].index;
        p.contentGroup = static_cast<std::uint8_t>(catalog::releases::unresolved);
        if (const auto* rule = catalog::releases::lookup(rows[i])) {
            p.contentGroup = rule->release;
            p.experience = rule->experience;
            p.experienceKind = rule->kind;
            p.classificationSource = 4;
        }
    }
    (void)catalog::publish_presentations(std::span(presentation).first(rows.size()));
    catalog::MenuText menuText{};
    build_menu_text(source, scratch, globals, std::span(refs).first(rows.size()), menuText);
    (void)catalog::publish_menu_text(menuText);
}
} // namespace sunrise::client::content::activity
