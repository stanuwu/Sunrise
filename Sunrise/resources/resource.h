#pragma once

/** The first module-local RCDATA identifier owns the bundled default settings document. */
#define IDR_DEFAULT_SETTINGS 101
/** The next module-local RCDATA identifier embeds the required Dear ImGui MIT notice. */
#define IDR_IMGUI_LICENSE 102
/** The next module-local RCDATA identifier embeds the required Microsoft Detours notice. */
#define IDR_DETOURS_LICENSE 103
/** The next module-local RCDATA identifier holds the animated logo sprite sheet, as a PNG. */
#define IDR_LOGO_SHEET 104
/** The next module-local RCDATA identifier embeds the required PUC-Lua MIT notice. */
#define IDR_LUA_LICENSE 105

/** User-authored bootflow DDS files consumed by the runtime TagHash override. */
#define IDR_BOOTFLOW_TEXTURE_80A145FF 2007
#define IDR_BOOTFLOW_TEXTURE_80A14601 2008
#define IDR_BOOTFLOW_TEXTURE_80A14607 2010
#define IDR_BOOTFLOW_TEXTURE_80A1460E 2012
#define IDR_BOOTFLOW_TEXTURE_80A1461D 2014
#define IDR_BOOTFLOW_TEXTURE_80A1461F 2015
#define IDR_BOOTFLOW_TEXTURE_80A14621 2016
#define IDR_BOOTFLOW_TEXTURE_80A14624 2017
#define IDR_BOOTFLOW_TEXTURE_80A14625 2018
#define IDR_BOOTFLOW_TEXTURE_80A14628 2019
#define IDR_BOOTFLOW_TEXTURE_80A14629 2020
#define IDR_BOOTFLOW_TEXTURE_80A1462B 2021
#define IDR_BOOTFLOW_TEXTURE_80A1462E 2022
#define IDR_BOOTFLOW_TEXTURE_80A1462F 2023
#define IDR_BOOTFLOW_TEXTURE_80A14631 2024
#define IDR_BOOTFLOW_TEXTURE_80A14633 2025
#define IDR_BOOTFLOW_TEXTURE_80A14636 2026
#define IDR_BOOTFLOW_TEXTURE_80A146D5 2031

/** The four numeric fields of the version resource, in FILEVERSION order. */
#define SUNRISE_VER_MAJOR 0
#define SUNRISE_VER_MINOR 5
#define SUNRISE_VER_PATCH 1
#define SUNRISE_VER_BUILD 0
/** The same version as display text. Windows shows this string, not the four fields. */
#define SUNRISE_VER_STRING "0.5.1.0"

/** SQLite schema, default rows, and upstream notice are embedded in the DLL. */
#define IDR_INVESTMENT_SCHEMA 106
#define IDR_INVESTMENT_DEFAULTS 107
#define IDR_SQLITE_NOTICE 108

/** Account preferences have their own schema and seed data in the save. */
#define IDR_ACCOUNT_SETTINGS_SCHEMA 109
#define IDR_ACCOUNT_SETTINGS_DEFAULTS 110
/** Build-scoped vendor turn-in definitions are static content, not player-save rows. */
#define IDR_VENDOR_REPUTATION_CONTENT 111
