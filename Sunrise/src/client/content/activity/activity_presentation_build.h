#pragma once

#include <cstring>

#include "../../../middleware/content/packages/reader/layout.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/reader/texture_header.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808029.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80804a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80805b.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80805e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80805f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809a.h"
#include "../../../middleware/content/packages/tables/activity_presentation_table.h"
#include "../../../state/build_data/activities/activity_artwork.h"
#include "../../../state/build_data/activities/activity_releases.h"
#include "activity_menu_text_build.h"

namespace sunrise::client::content::activity {
/**
 * Finds the frame rows of one sequence of an icon bitmap: data -> sequences[group].frames.
 * @param bytes Icon bitmap tag bytes.
 * @param group Sequence index.
 * @param groups Receives the sequence count.
 * @param frames Receives that sequence's frame rows; each row holds one texture tag.
 * @return False when the data object, the sequence or its frame rows are not in the blob.
 */
[[nodiscard]] inline bool
icon_bitmap_frames(std::span<const std::byte> bytes,
                   std::size_t group,
                   std::uint64_t& groups,
                   middleware::content::packages::tables::Array& frames) noexcept {
    namespace packages = middleware::content::packages;
    namespace tables = packages::tables;
    namespace generated = packages::schema::generated;
    // Both data classes share one layout; only their row classes differ.
    static_assert(offsetof(generated::C80804a67, sequences)
                  == offsetof(generated::C80804a65, sequences));
    static_assert(sizeof(generated::C80804a6c) == sizeof(generated::C80804a6a));
    static_assert(offsetof(generated::C80804a6c, frames) == offsetof(generated::C80804a6a, frames));
    static_assert(sizeof(generated::CuiBitmapFrame) == sizeof(generated::CuiBitmapFrameAlt));
    // A class word precedes the bitmap data record.
    constexpr std::size_t kClassWord = packages::reader::layout::kClassWordSize;
    std::size_t data{};
    std::uint32_t kind{};
    if (!tables::relative(bytes, offsetof(generated::SIconBitmap, data), data) || data < kClassWord
        || !tables::read(bytes, data - kClassWord, kind)
        || (kind != generated::C80804a67::kClass && kind != generated::C80804a65::kClass)) {
        return false;
    }
    const bool alternate = kind == generated::C80804a65::kClass;
    const std::uint32_t sequenceClass =
        alternate ? generated::C80804a6a::kClass : generated::C80804a6c::kClass;
    const std::uint32_t frameClass =
        alternate ? generated::CuiBitmapFrameAlt::kClass : generated::CuiBitmapFrame::kClass;
    tables::Array sequences{};
    if (!tables::read_array(bytes,
                            data + offsetof(generated::C80804a67, sequences),
                            sequenceClass,
                            sizeof(generated::C80804a6c),
                            sequences)
        || group >= sequences.count
        || !tables::read_array(bytes,
                               sequences.dataOffset + group * sizeof(generated::C80804a6c)
                                   + offsetof(generated::C80804a6c, frames),
                               frameClass,
                               sizeof(generated::CuiBitmapFrame),
                               frames)) {
        return false;
    }
    groups = sequences.count;
    return true;
}

/** Native Director campaign/activity styles, verified against the installed style strings.
 * Style layout -> icon row 2 (82px colored selection shield) -> bitmap sequence 0, frame 0 ->
 * RGBA texture. These are artwork identities, never destination overrides or launch IDs. */
inline void build_artwork(const middleware::content::packages::reader::Source& source,
                          middleware::content::packages::reader::Scratch& scratch) noexcept {
    namespace packages = middleware::content::packages;
    namespace catalog = state::build_data::activities;
    namespace parser = packages::tables::activities;
    namespace generated = packages::schema::generated;
    // Style tags index the fixed artwork table.
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
        std::uint32_t buffer{};
        packages::reader::TextureHeader header{};
        if (!packages::reader::read_tag(source, scratch, tag, bytes, buffer)
            || bytes.size() != sizeof header) {
            return;
        }
        std::memcpy(&header, bytes.data(), sizeof header);
        const std::uint16_t width = header.width, height = header.height;
        if (header.format != packages::reader::kTextureFormatRgba8
            || header.marker != packages::reader::kTextureHeaderMarker || width == 0 || height == 0
            || width > 256 || height > 256 || header.depth != 1 || header.arraySize != 1
            || header.largeBuffer != packages::reader::kNoLargeBuffer) {
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
        std::uint64_t groups{};
        packages::tables::Array variants{}, frames{};
        if (!read(styles[i], generated::DirectorStyle::kClass)
            || !parser::read(bytes, offsetof(generated::DirectorStyle, styleLayout), tag)
            || !read(tag, generated::SIconLibrary::kClass)
            || !packages::tables::find_array_at(
                bytes, offsetof(generated::SIconLibrary, icons), variants)
            || variants.count < 3 || variants.count > 16
            || !parser::read(bytes,
                             variants.dataOffset + 2 * sizeof(generated::SIconLibraryRow)
                                 + offsetof(generated::SIconLibraryRow, bitmap0),
                             tag)
            || !read(tag, generated::SIconBitmap::kClass)
            || !icon_bitmap_frames(bytes, 0, groups, frames) || frames.count == 0
            || !parser::read(
                bytes, frames.dataOffset + offsetof(generated::CuiBitmapFrame, texture), tag)) {
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
        std::uint8_t field{offsetof(generated::SIconContainer, iconBitmap0PrimaryBitmap)},
            variant{};
    };
    // The season mark is the icon container's fourth bitmap.
    constexpr std::uint8_t kSeasonMarkField = offsetof(generated::SIconContainer, iconBitmap3);
    // Each authored icon index must resolve to its expected texture.
    constexpr std::array<NativeIcon, 13> eventIcons{
        {// The 32px season-table marks are smaller than the standalone 45px inventory marks.
         // Variant 3 is the complete authored glyph; 96px variants are corner tiles with a smaller
         // glyph.
         {catalog::Icon{28}, 12109, 0x813217EC, kSeasonMarkField, 3},
         {catalog::Icon{29}, 13329, 0x81A2760C, kSeasonMarkField, 3},
         {catalog::Icon{30}, 13610, 0x81A27409, kSeasonMarkField, 3},
         {catalog::Icon{31}, 14737, 0x81A27487, kSeasonMarkField, 3},
         {catalog::Icon{32}, 10586, 0x8132D4B0},
         {catalog::Icon{33}, 5973, 0x8132D6B0},
         {catalog::Icon{34}, 5964, 0x8132D573},
         {catalog::Icon{35}, 5980, 0x8132D4E6},
         {catalog::Icon{36}, 11203, 0x8132D914},
         {catalog::Icon{37}, 16, 0x81320DB2},
         {catalog::Icon{38}, 10541, 0x813185CC, kSeasonMarkField, 3},
         {catalog::Icon{39}, 11305, 0x813217A8, kSeasonMarkField, 3},
         {catalog::Icon{40}, 11578, 0x813217CA, kSeasonMarkField, 3}}};
    packages::tables::Array iconRows{};
    if (read(0x81A291C2, generated::IconMap::kClass)
        && packages::tables::find_array_at(bytes, offsetof(generated::IconMap, table), iconRows)
        && iconRows.count <= 20000 && iconRows.dataOffset <= bytes.size()
        && iconRows.count <= (bytes.size() - iconRows.dataOffset) / sizeof(generated::C80802957)) {
        std::vector<std::byte> iconMap;
        iconMap.swap(bytes);
        for (const auto& icon : eventIcons) {
            std::uint32_t tag{};
            std::uint64_t groups{};
            packages::tables::Array frames{};
            if (icon.index >= iconRows.count
                || !parser::read(iconMap,
                                 iconRows.dataOffset + icon.index * sizeof(generated::C80802957)
                                     + offsetof(generated::C80802957, unk10),
                                 tag)
                || !read(tag, generated::SIconContainer::kClass)
                || !parser::read(bytes, icon.field, tag)
                || !read(tag, generated::SIconBitmap::kClass)
                || !icon_bitmap_frames(bytes, 0, groups, frames) || groups != 1
                || frames.count <= icon.variant || frames.count > 16
                || !parser::read(bytes,
                                 frames.dataOffset
                                     + icon.variant * sizeof(generated::CuiBitmapFrame)
                                     + offsetof(generated::CuiBitmapFrame, texture),
                                 tag)
                || tag != icon.expectedTexture) {
                continue;
            }
            texture(static_cast<std::size_t>(icon.icon), tag);
        }
    }
    (void)catalog::publish_artwork(std::move(images));
}

/** Publishes localized activity text and release groupings for the installed activity rows. */
inline void
build_presentations(const middleware::content::packages::reader::Source& source,
                    middleware::content::packages::reader::Scratch& scratch,
                    std::span<const std::byte> globals,
                    std::span<const state::build_data::activities::Definition> rows) noexcept {
    namespace packages = middleware::content::packages;
    namespace catalog = state::build_data::activities;
    namespace tables = packages::tables;
    namespace parser = tables::activities;
    namespace generated = packages::schema::generated;
    std::vector<std::byte> display, types, banks, container, language;
    using Globals = generated::InvestmentGlobals;
    const auto child =
        [&](std::size_t field, std::uint32_t expected, std::vector<std::byte>& target) noexcept {
            std::uint32_t tag{}, actual{};
            return tables::read(globals, field, tag)
                   && packages::reader::read_tag(source, scratch, tag, target, actual)
                   && actual == expected;
        };
    static std::array<parser::DisplayRefs, catalog::kCapacity> refs{};
    static std::array<catalog::Presentation, catalog::kCapacity> presentation{};
    tables::Array bankRows{};
    if (!child(offsetof(Globals, activityDisplayTable),
               generated::ActivityClientTable::kClass,
               display)
        || !child(offsetof(Globals, activityTypeTable), generated::ActivityTypeTable::kClass, types)
        || !child(offsetof(Globals, stringBankIndex), generated::StringBankIndex::kClass, banks)
        || !tables::find_array_at(banks, offsetof(generated::StringBankIndex, table), bankRows)
        || bankRows.count > 8192 || bankRows.dataOffset > banks.size()
        || bankRows.count > (banks.size() - bankRows.dataOffset) / sizeof(generated::StringBankRow)
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
        if (!parser::read(banks,
                          bankRows.dataOffset + bank * sizeof(generated::StringBankRow)
                              + offsetof(generated::StringBankRow, container),
                          tag)
            || !packages::reader::read_tag(source, scratch, tag, container, actual)
            || actual != generated::SLocalizedStrings::kClass
            || !parser::read(
                container, offsetof(generated::SLocalizedStrings, languageData), english)
            || !packages::reader::read_tag(source, scratch, english, language, actual)
            || actual != generated::SLocalizedStringsData::kClass) {
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
        p.movie = refs[i].movie;
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
