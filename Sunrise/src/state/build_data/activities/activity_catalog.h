#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::state::build_data::activities {
inline constexpr std::size_t kCapacity = 4095;
inline constexpr std::size_t kContentCount = 23, kExperienceKindCount = 7;
struct NativeText {
    std::array<char, 96> text{};
    // Bank ordinal, or direct native string-container tag for Director style labels.
    std::uint32_t bank{0xFFFF}, hash{0x811C9DC5};
};
struct MenuText {
    std::array<NativeText, kContentCount> content{};
    std::array<NativeText, kExperienceKindCount> kinds{};
};
/** Optional runtime localization. Classification tables never supply display text. */
[[nodiscard]] bool publish_menu_text(const MenuText& value) noexcept;
/** @return Published labels, or an empty immutable value while extraction is pending. */
[[nodiscard]] const MenuText& menu_text() noexcept;
struct Definition {
    std::uint16_t index{};
    std::uint32_t hash{};
    std::uint32_t gameplaySettingsHash{};
    std::uint8_t nativeType{};
    std::uint8_t destination{};
    std::array<char, 40> package{};
    [[nodiscard]] std::string_view name() const noexcept {
        return package.data();
    }
};
struct Presentation {
    std::array<char, 160> title{};
    std::array<char, 1024> description{};
    std::array<char, 64> type{};
    std::array<char, 64> expansion{};
    std::uint32_t nativeStyle{};
    std::uint16_t experience{0xFFFF};
    std::uint8_t experienceKind{};
    /** Original release index; releases::unresolved when not established. */
    std::uint8_t contentGroup{};
    std::uint8_t classificationSource{}; // 4 explicit local activity-hash release map
};
/** Optional, immutable Director text. Kept outside the persistent scenario cache. */
[[nodiscard]] bool publish_presentations(std::span<const Presentation> rows) noexcept;
/** @return The indexed presentation, or an empty immutable value when unavailable. */
[[nodiscard]] const Presentation& presentation(std::uint16_t index) noexcept;
/** Immutable process-local public activity table, independent of the persistent scenario cache. */
[[nodiscard]] bool ready() noexcept;
/** @return True when extraction failed and no catalog was published during this process. */
[[nodiscard]] bool extraction_failed() noexcept;
/** Optional menu data must not keep core investment startup waiting. */
void note_extraction_failure() noexcept;
/** Publishes validated dense identities once from the investment worker. Never replaces rows. */
[[nodiscard]] bool publish(std::span<const Definition> rows) noexcept;
/** @return Immutable process-lifetime identities, or an empty span before publication. */
[[nodiscard]] std::span<const Definition> entries() noexcept;
} // namespace sunrise::state::build_data::activities
