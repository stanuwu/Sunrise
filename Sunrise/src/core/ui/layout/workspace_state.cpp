#include "workspace_state.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace sunrise::core::ui::layout::workspace {
namespace {
float bounded(float value, float low, float high) noexcept {
    return std::clamp(std::isfinite(value) ? value : low, low, high);
}
bool valid_id(std::string_view id) noexcept {
    return id.size() <= 48 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'
                      || c == '-';
           });
}
struct Reader {
    std::string_view text;
    void whitespace() noexcept {
        while (!text.empty()
               && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r'
                   || text.front() == '\t'))
            text.remove_prefix(1);
    }
    bool take(char c) noexcept {
        whitespace();
        if (text.empty() || text.front() != c) return false;
        text.remove_prefix(1);
        return true;
    }
    bool string(std::string_view& value) noexcept {
        if (!take('"')) return false;
        const auto end = text.find('"');
        if (end == std::string_view::npos) return false;
        value = text.substr(0, end);
        text.remove_prefix(end + 1);
        return true;
    }
    bool number(float& value) noexcept {
        whitespace();
        const auto end = text.find_first_of(",} \t\r\n");
        const auto token = text.substr(0, end);
        if (token.empty()) return false;
        // Enforce JSON's number grammar before converting (no NaN, +1, .5 or leading zeroes).
        std::size_t i = token.front() == '-' ? 1 : 0;
        if (i == token.size()) return false;
        if (token[i] == '0')
            ++i;
        else {
            if (token[i] < '1' || token[i] > '9') return false;
            while (i < token.size() && token[i] >= '0' && token[i] <= '9')
                ++i;
        }
        if (i < token.size() && token[i] == '.') {
            const auto start = ++i;
            while (i < token.size() && token[i] >= '0' && token[i] <= '9')
                ++i;
            if (i == start) return false;
        }
        if (i < token.size() && (token[i] == 'e' || token[i] == 'E')) {
            ++i;
            if (i < token.size() && (token[i] == '+' || token[i] == '-')) ++i;
            const auto start = i;
            while (i < token.size() && token[i] >= '0' && token[i] <= '9')
                ++i;
            if (i == start) return false;
        }
        if (i != token.size()) return false;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()
            || !std::isfinite(value))
            return false;
        text.remove_prefix(token.size());
        return true;
    }
    bool boolean(bool& value) noexcept {
        whitespace();
        if (text.starts_with("true")) {
            value = true;
            text.remove_prefix(4);
            return true;
        }
        if (text.starts_with("false")) {
            value = false;
            text.remove_prefix(5);
            return true;
        }
        return false;
    }
};
} // namespace

Rect fit(Rect rect, float width, float height) noexcept {
    width = (std::max)(width, 1.0F);
    height = (std::max)(height, 1.0F);
    rect.width = bounded(rect.width, (std::min)(420.0F, width), width);
    rect.height = bounded(rect.height, (std::min)(300.0F, height), height);
    rect.x = bounded(rect.x, 0, width - rect.width);
    rect.y = bounded(rect.y, 0, height - rect.height);
    return rect;
}
Rect defaults(float width, float height) noexcept {
    Rect rect =
        fit({0, 0, (std::min)(920.0F, width - 48), (std::min)(580.0F, height - 48)}, width, height);
    rect.x = (width - rect.width) / 2;
    rect.y = (height - rect.height) / 2;
    return rect;
}
Rect display(State& state, float width, float height) noexcept {
    if (!state.positioned) {
        state.restore = defaults(width, height);
        state.positioned = true;
    }
    state.restore = fit(state.restore, width, height);
    return state.maximized ? Rect{0, 0, width, height} : state.restore;
}
void reset(State& state, float width, float height) noexcept {
    state.restore = defaults(width, height);
    state.positioned = true;
    state.maximized = false;
}
bool open(State& state, std::string_view stableId) noexcept {
    if (stableId.empty() || !valid_id(stableId)) return false;
    for (std::size_t i = 0; i < state.tabCount; ++i) {
        if (std::string_view(state.tabs[i].data()) == stableId) {
            state.selected = state.tabs[i];
            return true;
        }
    }
    if (state.tabCount == kTabCapacity) return false;
    state.tabs[state.tabCount++] = id(stableId);
    state.selected = id(stableId);
    return true;
}
void ensure_main(State& state) noexcept {
    const auto main = id(kMainTabId);
    const auto begin = state.tabs.begin();
    auto end = begin + state.tabCount;
    auto found = std::find(begin, end, main);
    if (found == end) {
        if (state.tabCount == kTabCapacity) --state.tabCount;
        found = begin + state.tabCount++;
        *found = main;
    }
    std::rotate(begin, found, found + 1);
    end = begin + state.tabCount;
    if (std::find(begin, end, state.selected) == end) state.selected = main;
}
void close(State& state, std::size_t index) noexcept {
    if (index >= state.tabCount || std::string_view(state.tabs[index].data()) == kMainTabId) return;
    const bool active = state.selected == state.tabs[index];
    for (std::size_t i = index + 1; i < state.tabCount; ++i)
        state.tabs[i - 1] = state.tabs[i];
    state.tabs[--state.tabCount] = {};
    if (active)
        state.selected =
            state.tabCount == 0 ? TabId{} : state.tabs[(std::min)(index, state.tabCount - 1)];
}
void move(State& state, std::size_t from, std::size_t to) noexcept {
    if (from >= state.tabCount || to >= state.tabCount || from == to || from == 0 || to == 0)
        return;
    if (from < to)
        std::rotate(
            state.tabs.begin() + from, state.tabs.begin() + from + 1, state.tabs.begin() + to + 1);
    else
        std::rotate(
            state.tabs.begin() + to, state.tabs.begin() + from, state.tabs.begin() + from + 1);
}
void reconcile(State& state, std::span<const std::string_view> available) noexcept {
    for (std::size_t i = 0; i < state.tabCount;) {
        if (std::string_view(state.tabs[i].data()) == kMainTabId) {
            ++i;
            continue;
        }
        if (std::find(available.begin(), available.end(), state.tabs[i].data()) == available.end())
            close(state, i);
        else
            ++i;
    }
    ensure_main(state);
}
bool decode(std::string_view text, State& output) noexcept {
    if (text.size() >= kFileCapacity) return false;
    Reader reader{text};
    State candidate{};
    unsigned seen{};
    float version{};
    bool legacySidebar{};
    float legacyWidth{};
    candidate.tabs = {};
    candidate.tabCount = 0;
    candidate.selected = {};
    constexpr std::array keys{"version",
                              "x",
                              "y",
                              "width",
                              "height",
                              "maximized",
                              "sidebarVisible",
                              "sidebarWidth",
                              "selected",
                              "tabs"};
    if (!reader.take('{')) return false;
    do {
        std::string_view key;
        if (!reader.string(key) || !reader.take(':')) return false;
        const auto found = std::find(keys.begin(), keys.end(), key);
        if (found == keys.end()) return false;
        const auto index = static_cast<unsigned>(found - keys.begin());
        if ((seen & (1U << index)) != 0) return false;
        seen |= 1U << index;
        switch (index) {
        case 0:
            if (!reader.number(version) || (version != 1 && version != 2 && version != 3))
                return false;
            break;
        case 1:
            if (!reader.number(candidate.restore.x)) return false;
            break;
        case 2:
            if (!reader.number(candidate.restore.y)) return false;
            break;
        case 3:
            if (!reader.number(candidate.restore.width) || candidate.restore.width <= 0)
                return false;
            break;
        case 4:
            if (!reader.number(candidate.restore.height) || candidate.restore.height <= 0)
                return false;
            break;
        case 5:
            if (!reader.boolean(candidate.maximized)) return false;
            break;
        case 6:
            if (!reader.boolean(legacySidebar)) return false;
            break;
        case 7:
            if (!reader.number(legacyWidth)) return false;
            break;
        case 8: {
            std::string_view id;
            if (!reader.string(id) || !valid_id(id)) return false;
            std::copy(id.begin(), id.end(), candidate.selected.begin());
            break;
        }
        case 9:
            if (!reader.take('[')) return false;
            if (!reader.take(']')) {
                do {
                    std::string_view tab;
                    if (candidate.tabCount == kTabCapacity || !reader.string(tab) || tab.empty()
                        || !valid_id(tab))
                        return false;
                    for (std::size_t i = 0; i < candidate.tabCount; ++i)
                        if (std::string_view(candidate.tabs[i].data()) == tab) return false;
                    candidate.tabs[candidate.tabCount++] = id(tab);
                } while (reader.take(','));
                if (!reader.take(']')) return false;
            }
            break;
        default:
            return false;
        }
    } while (reader.take(','));
    if (!reader.take('}')) return false;
    reader.whitespace();
    if (!reader.text.empty()) return false;
    if (version == 1) {
        if (seen != 511) return false;
        candidate.selected = id(kMainTabId);
        candidate.tabs[0] = candidate.selected;
        candidate.tabCount = 1;
    } else {
        if ((version != 2 && version != 3) || seen != 831) return false;
        if (candidate.tabCount == 0) {
            if (candidate.selected[0] != 0) return false;
        } else if (std::find(candidate.tabs.begin(),
                             candidate.tabs.begin() + candidate.tabCount,
                             candidate.selected)
                   == candidate.tabs.begin() + candidate.tabCount)
            return false;
        if (version == 2) {
            // The first browser-style workspace treated every menu page as a tab. Retain its
            // launched-tool identities, but return the revised shell to Sunrise's main layout.
            candidate.selected = id(kMainTabId);
            ensure_main(candidate);
        }
    }
    candidate.positioned = true;
    output = candidate;
    return true;
}
bool encode(const State& state, std::array<char, kFileCapacity>& output) noexcept {
    if (state.selected.back() != '\0' || !valid_id(state.selected.data())) return false;
    if (state.tabCount > kTabCapacity) return false;
    for (std::size_t i = 0; i < state.tabCount; ++i)
        if (state.tabs[i].back() != 0 || !valid_id(state.tabs[i].data())) return false;
    char* cursor = output.data();
    char* end = output.data() + output.size() - 1;
    bool ok = true;
    const auto append = [&](std::string_view text) {
        if (text.size() > static_cast<std::size_t>(end - cursor)) {
            ok = false;
            return;
        }
        cursor = std::copy(text.begin(), text.end(), cursor);
    };
    const auto number = [&](float value) {
        const auto result = std::to_chars(cursor, end, value);
        if (result.ec != std::errc{}) {
            ok = false;
            return;
        }
        cursor = result.ptr;
    };
    append("{\n  \"version\": 3,\n  \"x\": ");
    number(state.restore.x);
    append(", \"y\": ");
    number(state.restore.y);
    append(", \"width\": ");
    number(state.restore.width);
    append(", \"height\": ");
    number(state.restore.height);
    append(",\n  \"maximized\": ");
    append(state.maximized ? "true" : "false");
    append(",\n  \"selected\": \"");
    append(state.selected.data());
    append("\",\n  \"tabs\": [");
    for (std::size_t i = 0; i < state.tabCount; ++i) {
        if (i != 0) append(", ");
        append("\"");
        append(state.tabs[i].data());
        append("\"");
    }
    append("]\n}\n");
    *cursor = 0;
    State check{};
    return ok && decode({output.data(), static_cast<std::size_t>(cursor - output.data())}, check);
}
} // namespace sunrise::core::ui::layout::workspace
