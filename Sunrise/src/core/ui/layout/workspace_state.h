#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
namespace sunrise::core::ui::layout::workspace {
inline constexpr std::size_t kTabCapacity = 32;
inline constexpr std::size_t kFileCapacity = 4096;
inline constexpr std::string_view kMainTabId = "core.sunrise";
using TabId = std::array<char, 49>;
constexpr TabId id(std::string_view value) noexcept {
    TabId result{};
    if (value.size() <= 48) std::copy(value.begin(), value.end(), result.begin());
    return result;
}
struct Rect {
    float x{}, y{}, width{920}, height{580};
    bool operator==(const Rect&) const = default;
};
/** Persisted geometry and ordered stable tool IDs. */
struct State {
    Rect restore{};
    bool positioned{};
    bool maximized{};
    TabId selected{id(kMainTabId)};
    std::array<TabId, kTabCapacity> tabs{id(kMainTabId)};
    std::size_t tabCount{1};
    bool operator==(const State&) const = default;
};
Rect fit(Rect rect, float width, float height) noexcept;
Rect defaults(float width, float height) noexcept;
Rect display(State& state, float width, float height) noexcept;
void reset(State& state, float width, float height) noexcept;
bool open(State& state, std::string_view stableId) noexcept;
void ensure_main(State& state) noexcept;
void close(State& state, std::size_t index) noexcept;
void move(State& state, std::size_t from, std::size_t to) noexcept;
void reconcile(State& state, std::span<const std::string_view> available) noexcept;
/** Strict bounded JSON; older browser sessions regain the fixed Sunrise home view. */
bool decode(std::string_view text, State& output) noexcept;
bool encode(const State& state, std::array<char, kFileCapacity>& output) noexcept;
void load(State& state) noexcept;
bool save(const State& state) noexcept;
} // namespace sunrise::core::ui::layout::workspace
