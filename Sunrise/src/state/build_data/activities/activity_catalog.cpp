#include "activity_catalog.h"

#include <algorithm>
#include <atomic>

#include "activity_artwork.h"

namespace sunrise::state::build_data::activities {
namespace {
std::array<Definition, kCapacity> g_rows{};
std::atomic_size_t g_count{};
std::atomic_bool g_failed{};
std::array<Presentation, kCapacity> g_presentations{};
std::atomic_size_t g_presentationCount{};
std::array<Artwork, kIconCount> g_artwork{};
std::atomic_bool g_artReady{};
MenuText g_menuText{};
std::atomic_bool g_menuTextReady{};
} // namespace
bool publish_menu_text(const MenuText& value) noexcept {
    if (g_menuTextReady.load(std::memory_order_acquire)) {
        return false;
    }
    for (const auto& row : value.content) {
        if (row.text.back() != '\0') {
            return false;
        }
    }
    for (const auto& row : value.kinds) {
        if (row.text.back() != '\0') {
            return false;
        }
    }
    g_menuText = value;
    g_menuTextReady.store(true, std::memory_order_release);
    return true;
}
const MenuText& menu_text() noexcept {
    static const MenuText absent{};
    return g_menuTextReady.load(std::memory_order_acquire) ? g_menuText : absent;
}
bool ready() noexcept {
    return g_count.load(std::memory_order_acquire) != 0;
}
bool publish_artwork(std::array<Artwork, kIconCount>&& rows) noexcept {
    if (g_artReady.load(std::memory_order_acquire)) {
        return false;
    }
    for (const auto& row : rows) {
        if (!row.pixels.empty()
            && (row.width == 0 || row.height == 0 || row.width > 256 || row.height > 256
                || row.pixels.size() != static_cast<std::size_t>(row.width) * row.height * 4)) {
            return false;
        }
    }
    g_artwork = std::move(rows);
    g_artReady.store(true, std::memory_order_release);
    return true;
}
std::span<const Artwork> artwork() noexcept {
    return g_artReady.load(std::memory_order_acquire) ? std::span<const Artwork>(g_artwork)
                                                      : std::span<const Artwork>{};
}
bool publish_presentations(std::span<const Presentation> rows) noexcept {
    if (g_presentationCount.load(std::memory_order_acquire) != 0 || rows.empty()
        || rows.size() > kCapacity) {
        return false;
    }
    for (const auto& row : rows) {
        if (row.title.back() != '\0' || row.description.back() != '\0' || row.type.back() != '\0') {
            return false;
        }
    }
    std::copy(rows.begin(), rows.end(), g_presentations.begin());
    g_presentationCount.store(rows.size(), std::memory_order_release);
    return true;
}
const Presentation& presentation(std::uint16_t index) noexcept {
    static const Presentation absent{};
    return index < g_presentationCount.load(std::memory_order_acquire) ? g_presentations[index]
                                                                       : absent;
}
bool extraction_failed() noexcept {
    return !ready() && g_failed.load(std::memory_order_acquire);
}
void note_extraction_failure() noexcept {
    g_failed.store(true, std::memory_order_release);
}
bool publish(std::span<const Definition> rows) noexcept {
    // The cooperative investment worker is the sole producer. Never replace published storage.
    if (ready() || rows.empty() || rows.size() > g_rows.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].index != i || rows[i].hash == 0 || rows[i].package.back() != '\0') {
            return false;
        }
    }
    std::copy(rows.begin(), rows.end(), g_rows.begin());
    g_count.store(rows.size(), std::memory_order_release);
    return true;
}
std::span<const Definition> entries() noexcept {
    return {g_rows.data(), g_count.load(std::memory_order_acquire)};
}
} // namespace sunrise::state::build_data::activities
