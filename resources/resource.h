#pragma once

/** The first module-local RCDATA identifier owns the bundled default settings document. */
#define IDR_DEFAULT_SETTINGS 101
/** The next module-local RCDATA identifier embeds the required Dear ImGui MIT notice. */
#define IDR_IMGUI_LICENSE 102
/** The next module-local RCDATA identifier embeds the required Microsoft Detours notice. */
#define IDR_DETOURS_LICENSE 103

/** The four numeric fields of the version resource, in FILEVERSION order. */
#define SUNRISE_VER_MAJOR 0
#define SUNRISE_VER_MINOR 2
#define SUNRISE_VER_PATCH 1
#define SUNRISE_VER_BUILD 0
/** The same version as display text. Windows shows this string, not the four fields. */
#define SUNRISE_VER_STRING "0.2.1.0"
