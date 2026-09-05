#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "../Sunrise/src/core/filesystem/path.h"
#include "../Sunrise/src/core/logging/log.h"
#include "../Sunrise/src/core/ui/layout/workspace_state.h"
namespace fs = std::filesystem;
static fs::path directory;
static int warnings{};
namespace sunrise::core::log {
void write(Channel, Level, std::string_view) noexcept {
    ++warnings;
}
} // namespace sunrise::core::log
namespace sunrise::core::path {
bool artifact_file(std::wstring_view relative, Buffer& output) noexcept {
    const auto text = (directory / relative).wstring();
    if (text.size() >= output.chars.size()) return false;
    std::copy(text.begin(), text.end(), output.chars.begin());
    output.length = text.size();
    output.chars[text.size()] = 0;
    return true;
}
bool read_artifact_text(std::wstring_view relative, std::span<char> text) noexcept {
    std::ifstream file(directory / relative, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(file)), {});
    if (data.empty() || data.size() >= text.size()) return false;
    std::copy(data.begin(), data.end(), text.begin());
    text[data.size()] = 0;
    return true;
}
} // namespace sunrise::core::path
int main() {
    using namespace sunrise::core::ui::layout::workspace;
    directory = fs::temp_directory_path()
                / ("sunrise-workspace-test-" + std::to_string(GetCurrentProcessId()));
    assert(fs::create_directory(directory));
    State state{};
    load(state);
    assert(!state.positioned);
    (void)display(state, 1920, 1080);
    assert(save(state));
    State loaded{};
    load(loaded);
    assert(loaded == state);
    state.maximized = true;
    (void)open(state, "server.packets");
    assert(save(state));
    load(loaded);
    assert(loaded == state);
    // Deny replacement of the target: the prior complete file must survive.
    HANDLE locked = CreateFileW((directory / "workspace.json").c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    (void)open(state, "client.movement");
    assert(!save(state));
    assert(!save(state));
    assert(warnings == 1);
    CloseHandle(locked);
    State prior{};
    load(prior);
    assert(prior == loaded);
    assert(save(state));
    load(loaded);
    assert(loaded == state);
    {
        std::ofstream file(directory / "workspace.json");
        file << "{bad";
    }
    State defaults{};
    load(defaults);
    assert(!defaults.positioned && defaults.tabCount == 1);
    std::array<char, kFileCapacity> valid{};
    assert(encode(state, valid));
    {
        std::ofstream file(directory / "workspace.json", std::ios::binary);
        file << valid.data();
        file.put(0);
        file << "junk";
    }
    load(defaults);
    assert(!defaults.positioned);
    fs::remove_all(directory);
}
