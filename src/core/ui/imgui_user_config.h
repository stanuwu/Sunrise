#pragma once

// Sunrise does not let Dear ImGui read or write files at runtime.
#define IMGUI_DISABLE_FILE_FUNCTIONS
// Every Dear ImGui allocation must use Sunrise's bounded static arena.
#define IMGUI_DISABLE_DEFAULT_ALLOCATORS
// The generic shell helper stays off; the credits badge owns its one URL action.
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
// ImGui never polls the clipboard; the Logs page owns one explicit copy action.
#define IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS
// The game stays the only input-method owner while Sunrise is loaded.
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
// The first UI milestone leaves all gamepad polling and ownership with the game.
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
