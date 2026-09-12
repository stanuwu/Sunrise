#pragma once
#include <array>
#include <cctype>
#include <cstdio>
#include <string_view>

#include "../../../state/build_data/activities/activity_artwork.h"
#include "../../../state/build_data/activities/activity_catalog.h"
#include "../../../state/build_data/activities/activity_releases.h"

namespace sunrise::client::ui::mission_launch {
using Activity = state::build_data::activities::Definition;
namespace releases = state::build_data::activities::releases;
struct ContentNames {
    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return state::build_data::activities::kContentCount;
    }
    [[nodiscard]] const char* operator[](std::size_t content) const noexcept {
        if (content == 0) {
            return "All content";
        }
        if (content == releases::unresolved) {
            return "Unclassified content";
        }
        const auto& names = state::build_data::activities::menu_text().content;
        return content < names.size() && names[content].text[0] ? names[content].text.data()
                                                                : "Content name unavailable";
    }
};
inline constexpr ContentNames kContent{};
inline constexpr auto kUnresolvedContent = releases::unresolved;
/** Launcher presentation preference only; the native catalog and release evidence remain intact. */
[[nodiscard]] inline bool content_visible(std::size_t content) noexcept {
    return content != 18 && content != 21; // Hidden content IDs.
}
[[nodiscard]] inline bool library_card_visible(std::size_t content) noexcept {
    return content_visible(content) && content != 17; // Content IDs 14 and 17 share one card.
}
[[nodiscard]] inline const char* library_name(std::size_t content) noexcept {
    return kContent[content];
}
[[nodiscard]] inline unsigned library_kind(std::size_t content) noexcept {
    if (content == kUnresolvedContent) {
        return 0;
    }
    if ((content >= 14 && content <= 17) || (content >= 19 && content <= 21)) {
        return 3;
    }
    return content >= 7 && content <= 13 ? 2 : 1;
}
/** Reviewed original-release evidence; runtime destination and variant subtitles do not identify
 * release. */
[[nodiscard]] inline std::size_t content_group(const Activity& activity) noexcept {
    const auto group = state::build_data::activities::presentation(activity.index).contentGroup;
    return group >= 1 && group < kContent.size() ? group : kUnresolvedContent;
}
[[nodiscard]] inline std::size_t library_group(const Activity& row) noexcept {
    const auto content = content_group(row);
    return content == 17
               ? 14
               : content; // Content IDs 14 and 17 share navigation, retaining their launch IDs.
}
[[nodiscard]] inline std::size_t activity_type(const Activity& activity) noexcept {
    const auto kind = state::build_data::activities::presentation(activity.index).experienceKind;
    if (kind == 3) {
        return 3;
    }
    if (kind == 5) {
        return 5;
    }
    switch (activity.nativeType) {
    case 0:
    case 1:
    case 2:
    case 11:
    case 52:
        return 1;
    case 3:
    case 4:
    case 5:
    case 12:
    case 39:
        return 2;
    case 7:
        return 4;
    case 46:
        return 5;
    case 9:
    case 20:
        return 6;
    case 14:
    case 15:
    case 16:
    case 17:
    case 19:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 36:
    case 38:
    case 41:
    case 42:
    case 43:
    case 47:
    case 48:
    case 49:
    case 50:
    case 51:
        return 7;
    default:
        return 8;
    }
}
[[nodiscard]] inline bool contains(std::string_view text, std::string_view query) noexcept {
    if (query.empty()) {
        return true;
    }
    if (query.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i <= text.size() - query.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; match && j < query.size(); ++j) {
            match = std::tolower(static_cast<unsigned char>(text[i + j]))
                    == std::tolower(static_cast<unsigned char>(query[j]));
        }
        if (match) {
            return true;
        }
    }
    return false;
}
[[nodiscard]] inline std::array<char, 160> title(const Activity& activity) noexcept {
    const auto& display = state::build_data::activities::presentation(activity.index);
    if (display.title[0] != '\0') {
        return display.title;
    }
    std::array<char, 160> result{};
    const auto name = activity.name();
    if (name.empty()) {
        (void)std::snprintf(
            result.data(), result.size(), "Activity %u - no direct destination", activity.index);
        return result;
    }
    (void)std::snprintf(result.data(), result.size(), "%s", activity.package.data());
    return result;
}
[[nodiscard]] inline state::build_data::activities::Icon
activity_icon(const Activity& activity) noexcept {
    using Icon = state::build_data::activities::Icon;
    const auto& display = state::build_data::activities::presentation(activity.index);
    switch (content_group(activity)) {
    case 14:
    case 17:
        return Icon{34};
    case 15:
        return Icon{33};
    case 16:
        return Icon{36};
    case 21:
        return Icon{37};
    default:
        break;
    }
    switch (display.nativeStyle) {
    case 0x9BD15777:
        return Icon{14};
    case 0x0ABD9396:
        return Icon{22};
    case 0xE72921B0:
        return Icon{23};
    case 0xCCA2A4D8:
        return Icon{24};
    case 0xAD35C5A9:
        return Icon{25};
    case 0xEC411540:
        return Icon{26};
    case 0x6B5D0665:
        return Icon{27};
    default:
        break;
    }
    if (display.experienceKind == 3) {
        return Icon{14};
    }
    if (display.experienceKind == 5) {
        return Icon{9};
    }
    if (display.experienceKind == 6) {
        return Icon{27};
    }
    switch (activity.nativeType) {
    case 0:
    case 1:
    case 2:
    case 11:
    case 52:
        return content_group(activity) <= 6 ? static_cast<Icon>(content_group(activity)) : Icon{0};
    case 3:
    case 4:
    case 5:
    case 12:
    case 39:
        return Icon{7};
    case 7:
        return Icon{8};
    case 9:
        return Icon{10};
    case 20:
        return Icon{11};
    case 31:
    case 32:
    case 36:
    case 38:
        return Icon{13};
    case 35:
        return Icon{18};
    case 37:
        return Icon{21};
    case 40:
        return Icon{19};
    case 8:
    case 13:
    case 44:
        return Icon{16};
    case 45:
        return Icon{15};
    case 46:
        return Icon{9};
    case 53:
        return Icon{20};
    default:
        return activity_type(activity) == 7 ? Icon{12} : Icon{0};
    }
}
/** Presentation grouping never changes the selected public ID or native launch context. */
[[nodiscard]] inline std::uint16_t experience_id(const Activity& row) noexcept {
    const auto id = state::build_data::activities::presentation(row.index).experience;
    return id < state::build_data::activities::kCapacity ? id : row.index;
}
[[nodiscard]] inline std::string_view experience_type(const Activity& row) noexcept {
    const auto& p = state::build_data::activities::presentation(row.index);
    const auto& kinds = state::build_data::activities::menu_text().kinds;
    if (p.experienceKind > 0 && p.experienceKind < kinds.size()) {
        return kinds[p.experienceKind].text[0] ? kinds[p.experienceKind].text.data()
                                               : "Type unavailable";
    }
    return p.type[0] ? p.type.data() : "Type unavailable";
}
[[nodiscard]] inline int representative_score(const Activity& row, bool available) noexcept {
    const auto& p = state::build_data::activities::presentation(row.index);
    const auto* rule = releases::lookup(row);
    // Numeric variant preference is independent of localized display text.
    return (available ? 1000 : 0) + (p.title[0] ? 100 : 0)
           + (row.nativeType == 3 || row.nativeType == 0 ? 10 : 0)
           - (rule != nullptr ? rule->preferencePenalty : 0);
}
[[nodiscard]] inline state::build_data::activities::Icon
release_icon(std::size_t content) noexcept {
    using Icon = state::build_data::activities::Icon;
    if (content >= 1 && content <= 6) {
        return static_cast<Icon>(content);
    }
    // Authored season marks and recurring event emblems, resolved from native definitions.
    switch (content) {
    case 7:
        return Icon{38};
    case 8:
        return Icon{39};
    case 9:
        return Icon{40};
    case 10:
        return Icon{28};
    case 11:
        return Icon{29};
    case 12:
        return Icon{30};
    case 13:
        return Icon{31};
    case 14:
    case 17:
        return Icon{34};
    case 15:
        return Icon{33};
    case 16:
        return Icon{36};
    case 19:
        return Icon{35};
    case 20:
        return Icon{32};
    case 21:
        return Icon{37};
    default:
        return Icon{0};
    }
}
[[nodiscard]] inline bool matches(const Activity& activity,
                                  std::size_t content,
                                  std::size_t type,
                                  std::string_view query) noexcept {
    if ((content != 0 && library_group(activity) != content)
        || (type != 0 && activity_type(activity) != type)) {
        return false;
    }
    const auto name = title(activity);
    std::array<char, 40> identity{};
    (void)std::snprintf(identity.data(), identity.size(), "%u %08X", activity.index, activity.hash);
    return contains(activity.name(), query) || contains(name.data(), query)
           || contains(identity.data(), query);
}
} // namespace sunrise::client::ui::mission_launch
