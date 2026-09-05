# Diagnosing startup after SDK generation under UMU/Proton

This change adds diagnostics for [upstream issue #100](https://github.com/stanuwu/Sunrise/issues/100).
It does not establish or fix the cause of the reported exit code 5. The report provides neither
the Proton version nor a Wine exception trace, and the failing game environment has not been
reproduced here.

## What the source establishes

`Sunrise/src/state/runtime/state_runtime.cpp` emits `ev=account stage=identity` during normal
initialization, before session randomization and publication of State. The message is not an error.
It does not establish that every subsequent operation failed: each log channel has its own filter.

`Sunrise/src/core/runtime/core_runtime.cpp` then initializes the content manifest, authenticates
`Sunrise/sdk/catalog.bin`, and asks the SDK loader to read `Sunrise/activity_sdk.pack`. Middleware,
Server and Client initialization follow. A generated SDK therefore changes the startup work, but
that timing alone does not identify a corrupt cache or a runner bug.

`Sunrise/src/state/activity_sdk/activity_sdk_loader.cpp` opens and maps the pack read-only, checks
the expected identity, hashes the payload, and validates catalog relations before returning it.
Previously, many failures here only became a generic SDK status. The new events identify the
boundary and preserve the actual Win32 error on failed file/path/mapping calls. Expected first-boot
absence remains an informational `missing` result.

The patch retains the original startup order, validation checks, failure results and cleanup.
It neither catches crashes nor skips SDK validation. No game data is embedded or logged by the
new events. Logging changes timing, so a successful diagnostic run alone cannot prove a fix.

## Capture a failing run

1. Keep a backup of the existing DLL, settings, generated SDK and current logs before testing.
   Use the diagnostic `steam_api64.dll` built from this branch. Keep its matching PDB available
   for symbolizing a crash. The DLL is built by the normal Windows Release x64 workflow.
2. In `Sunrise/settings.json`, merge the following into the existing `core.logging` object.
   Preserve the other Core settings and the rest of the document; do not add duplicate keys or
   replace the whole settings file with this fragment.

   ```json
   {
     "debugger_sink": true,
     "file_sink": true,
     "levels": {
       "core": "debug",
       "state": "debug",
       "client": "debug",
       "server": "debug",
       "middleware": "debug"
     }
   }
   ```

3. Add `PROTON_LOG=1` to the environment of the existing failing UMU/Proton launch. Preserve its
   current executable, prefix, runner and DLL overrides. If launching through Steam, put
   `PROTON_LOG=1 %command%` in that game's launch options alongside any existing required options.
   [Proton documents](https://github.com/ValveSoftware/Proton#runtime-config-options) the log as
   `steam-<APPID>.log` under `PROTON_LOG_DIR`, which defaults to the user's home directory.
4. Reproduce once and copy `Sunrise/logs/sunrise.log`, the Proton log and the launcher terminal
   output before another launch rotates or overwrites them. Record the exact Sunrise commit/DLL,
   UMU and Proton versions, the vanilla Wine version that works, launch arguments, and whether the
   runners used the same prefix, game files and generated SDK. Review logs before sharing and
   redact credentials or personal paths if present; do not attach game files or SDK dumps.

## Read the result

- `ev=initialize stage=... phase=begin` followed by the matching `phase=complete` shows that a
  runtime stage returned. The last unmatched stage is a lead for the exception trace, not proof
  that its code caused the exception.
- `stage=catalog_manifest` and `stage=pack` separate catalog authentication from pack loading.
- State debug events name `open`, `pack_directory`, `file_size`, `file_mapping`, `map_view`,
  `header`, `payload_hash` and `catalog_validation`.
- `win32_error=...` is captured from an API that actually failed. A launcher's process exit code
  must not be interpreted as this field. Establish the exception/error domain from the Proton log.
- `stage=pack phase=complete result=ready` means pack loading and SDK publication returned; use
  the following startup stages and the exception trace to investigate a later crash.

If a cache-presence comparison is needed, use a disposable copy of the installation and preserve
the original generated files. Disable `core.activity_sdk_generation.enabled` in that copy before
temporarily moving its pack aside, so the comparison does not regenerate the SDK. A changed
outcome localizes the trigger; it still does not establish the underlying fault.

The next evidence needed for a behavioural fix is a failing Proton exception/backtrace paired
with these stage events and the exact runner and Sunrise versions.
