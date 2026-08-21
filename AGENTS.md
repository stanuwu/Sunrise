# Sunrise agent guide

## Project scope

Sunrise is a C++20 Windows DLL named `steam_api64.dll` that provides offline exploration for an
older Destiny 2 build. The public DLL/Steam shim entry points are in
[`Sunrise/src/dllmain.cpp`](Sunrise/src/dllmain.cpp). Read the project overview and supported build
paths in [`README.md`](README.md) before changing behavior.

## Source layout

- `core/` owns settings, logging, filesystem utilities, and shared UI/runtime orchestration.
- `client/` owns in-process hooks, executable-pattern discovery, game integration, and client UI.
- `server/` owns local service, BAP, HTTP, and gameplay runtime behavior.
- `state/` owns persistent in-memory game/account/content state and validation.
- `middleware/` owns protocol codecs, serialization, content/package reading, crypto, and transport
  primitives shared by higher layers.
- `steam/` implements the Steam-compatible process-local shim.
- `vendor/` contains reviewed upstream Detours and Dear ImGui sources. Do not edit or reformat them
  unless the task explicitly requires a vendor update; see [`THIRD_PARTY.md`](THIRD_PARTY.md).

Keep code in the narrowest owning layer. For behavior intended for the local server, prefer the
server request/push path over a client-side patch. Preserve dependency-safe lifecycle order: Core
initializes layers in order and unwinds them in reverse in
[`Sunrise/src/core/runtime/core_runtime.cpp`](Sunrise/src/core/runtime/core_runtime.cpp).

## Coding conventions

- Follow [`.clang-format`](.clang-format), [`.clang-tidy`](.clang-tidy), and
  [`.editorconfig`](.editorconfig): C++20, four spaces, LF, UTF-8, and a 100-column limit.
- Use the existing nested namespace convention, for example `sunrise::middleware::protobuf`; use
  quoted project-root includes such as `"middleware/protobuf/codec.h"` when crossing directories.
- Match adjacent code: public headers use `#pragma once`, APIs commonly use `[[nodiscard]]` and
  `noexcept`, and Doxygen-style comments document public behavior and failure contracts.
- Protocol, hook, and fixed-buffer code must validate input and capacity before committing output.
  Preserve the existing allocation-free/transactional patterns rather than adding implicit dynamic
  allocation or partial writes.
- Preserve offline/local behavior. Do not add game data to the repository; game data is extracted at
  runtime.

## Build and validation

Use the authoritative commands and prerequisites in [README — Building](README.md#building).

- On Windows, build `Release|x64` with VS 2026, toolset `v145`, and Windows SDK `10.0.26100`.
  CI uses `msbuild Sunrise.sln /m /p:Configuration=Release /p:Platform=x64`.
- Linux builds cross-compile with CMake/Ninja, Clang, xwin, and
  [`linux-to-win-toolchain.cmake`](linux-to-win-toolchain.cmake).
- The output DLL and PDB are `build/x64/<Configuration>/steam_api64.*`.
- No automated test target is currently tracked. For code changes, at minimum run the applicable
  build; format and clang-tidy changed project-owned files when those tools are available.

## Build-system and resource changes

- CMake recursively discovers `Sunrise/src` sources, but
  [`Sunrise/Sunrise.vcxproj`](Sunrise/Sunrise.vcxproj) explicitly lists `.cpp` and `.h` files. Add
  every new project-owned source/header to the `.vcxproj` so the Windows solution and CI build it.
- `Sunrise/resources/sunrise.rc` embeds the default settings, licenses, and artwork. Keep resource
  paths valid relative to that file and update project/resource metadata together when adding an
  embedded resource.
- Do not commit `build/`, `.vs/`, `.xwin-cache/`, IDE settings, or other ignored generated output.

## AGENT BEHAVIOR

- If the task requires out-of-scope/unreachable information that needs speculation/clarification
  indicate that to the user, either in the form of choice questions or requests for information.
  - e.g. some API call needs reverse engineering.
- Document every finding relating to project stucture, Destiny 2 codebase, network calls
  into `/docs` in a structured way so it's easy to find what the next person/agent is looking for.
  Try using simple technical english for that `(ASD-STE100)`.
- Try looking for relevant information in `/docs`.
