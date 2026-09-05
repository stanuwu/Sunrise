#include <cassert>
#include <limits>
#include <string>

#include "../Sunrise/src/core/ui/layout/workspace_state.h"
using namespace sunrise::core::ui::layout::workspace;
int main() {
    State state{};
    auto rect = display(state, 1920, 1080);
    assert(rect.x == 500 && rect.width == 920);
    assert(state.tabCount == 1 && std::string(state.selected.data()) == kMainTabId);
    assert(open(state, "client.movement") && open(state, "server.packets"));
    assert(open(state, "server.world") && state.tabCount == 4);
    move(state, 3, 1);
    assert(std::string(state.tabs[1].data()) == "server.world");
    close(state, 1);
    assert(std::string(state.selected.data()) == "client.movement");
    open(state, "client.player");
    open(state, "server.packets");
    close(state, 2);
    assert(std::string(state.selected.data()) == "client.player");
    close(state, 0);
    assert(state.tabCount == 3 && std::string(state.tabs[0].data()) == kMainTabId);
    move(state, 1, 0);
    assert(std::string(state.tabs[0].data()) == kMainTabId);
    const auto tabs = state.tabs;
    reset(state, 1920, 1080);
    assert(state.tabs == tabs);
    state.restore = {50, 60, 1000, 700};
    state.maximized = true;
    assert(display(state, 1920, 1080).width == 1920 && state.restore.x == 50);
    state.maximized = false;
    assert(display(state, 1920, 1080).width == 1000);
    rect = fit({9000, -20, 1000, 700}, 640, 480);
    assert(rect.x == 0 && rect.y == 0 && rect.width == 640);
    assert(fit({}, 200, 100).height == 100);
    assert(fit({std::numeric_limits<float>::infinity(), 0, 920, 580}, 1920, 1080).x == 0);
    std::array<char, kFileCapacity> json{};
    assert(encode(state, json));
    State loaded{};
    assert(decode(json.data(), loaded) && loaded == state);
    const auto original = loaded;
    for (const std::string& bad : std::initializer_list<std::string>{
             "{}",
             "null",
             std::string(json.data()) + "x",
             std::string(json.data()).replace(std::string(json.data()).find("1000"), 4, "NaN")})
        assert(!decode(bad, loaded) && loaded == original);
    for (const auto replacement : {"01", "+1", ".5", "1.", "1e999", "-1"}) {
        auto bad = std::string(json.data());
        bad.replace(bad.find("1000"), 4, replacement);
        assert(!decode(bad, loaded) && loaded == original);
    }
    const char* legacy =
        R"({"version":1,"x":100,"y":200,"width":800,"height":500,"maximized":true,"sidebarVisible":false,"sidebarWidth":200,"selected":"server.activity_host"})";
    assert(decode(legacy, loaded));
    assert(loaded.restore.x == 100 && loaded.maximized);
    assert(loaded.tabCount == 1 && std::string(loaded.selected.data()) == kMainTabId);
    const char* firstBrowserWorkspace =
        R"({"version":2,"x":10,"y":20,"width":900,"height":560,"maximized":false,"selected":"server.world","tabs":["server.world","server.packets"]})";
    assert(decode(firstBrowserWorkspace, loaded));
    assert(loaded.tabCount == 3 && std::string(loaded.tabs[0].data()) == kMainTabId);
    assert(std::string(loaded.selected.data()) == kMainTabId);
    State fixedHome{};
    std::array<std::string_view, 0> noHomeInCaller{};
    reconcile(fixedHome, noHomeInCaller);
    assert(fixedHome.tabCount == 1 && std::string(fixedHome.selected.data()) == kMainTabId);
    std::array<std::string_view, 2> available{kMainTabId, "client.player"};
    reconcile(state, available);
    assert(state.tabCount == 2 && std::string(state.selected.data()) == "client.player");
    close(state, 1);
    assert(state.tabCount == 1 && std::string(state.selected.data()) == kMainTabId);
    assert(encode(state, json) && decode(json.data(), loaded) && loaded.tabCount == 1);
    for (int i = 0; i < 31; ++i)
        assert(open(state, "test.tool" + std::to_string(i)));
    assert(!open(state, "test.extra"));
    assert(encode(state, json) && decode(json.data(), loaded));
    assert(loaded == state);
    State fullWithoutHome{};
    fullWithoutHome.tabs = {};
    fullWithoutHome.tabCount = 0;
    for (int i = 0; i < 32; ++i)
        fullWithoutHome.tabs[fullWithoutHome.tabCount++] = id("test.tool" + std::to_string(i));
    fullWithoutHome.selected = fullWithoutHome.tabs.back();
    ensure_main(fullWithoutHome);
    assert(fullWithoutHome.tabCount == 32
           && std::string(fullWithoutHome.tabs[0].data()) == kMainTabId
           && std::string(fullWithoutHome.selected.data()) == kMainTabId);
    auto duplicate = std::string(json.data());
    duplicate.insert(duplicate.find('}'), ", \"version\": 2");
    assert(!decode(duplicate, loaded));
    auto nullSuffix = std::string(json.data());
    nullSuffix.push_back(0);
    nullSuffix += "junk";
    assert(!decode(nullSuffix, loaded));
}
