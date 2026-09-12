# Experimental Linux/Proton runtime

Sunrise can run through Proton, but Linux runtime support is experimental. This guide assumes the
required Destiny 2 depots and the Sunrise `steam_api64.dll` have already been installed according
to the [manual installation guide](https://github.com/stanuwu/Sunrise/wiki/Installing).

The launcher has been tested with native Steam, Proton Experimental and a Btrfs home directory.
It does not download game data, install packages or modify a Steam-managed Destiny 2 installation.

## Requirements

- A 64-bit Linux system with working Vulkan drivers
- A native Steam installation
- Proton Experimental, Proton Hotfix or another compatible Proton build
- Bash, `realpath`, `cksum` and `flock` (normally provided by coreutils and util-linux)
- A separate directory containing the required historical game depots
- The Sunrise DLL installed as `<game>/bin/x64/steam_api64.dll`

Do not point the launcher at the current Steam-managed Destiny 2 installation. Sunrise requires the
historical game build described by the project's installation guide.

## Launching

From the repository root, run:

```bash
./scripts/linux/sunrise-run --game-dir "$HOME/Games/ProjectSunrise"
```

The launcher discovers Steam and Proton, creates an isolated prefix under the user data directory,
maps the game directory to Wine drive `D:`, and launches `D:\destiny2.exe`. Override discovery when
needed:

```bash
./scripts/linux/sunrise-run \
    --game-dir /path/to/ProjectSunrise \
    --proton "/path/to/Proton - Experimental/proton" \
    --steam-root /path/to/Steam \
    --prefix /path/to/compatdata
```

Run `./scripts/linux/sunrise-run --help` for the corresponding environment variables and all
options.

### Why the dedicated Wine drive is required

Proton normally translates a directly launched Linux path through Wine's `Z:` drive. A game stored
at `/home/user/Games/ProjectSunrise` can therefore see its executable and writable Sunrise directory
under `Z:\home\user\Games\ProjectSunrise`.

Sunrise validates every ancestor of its transactional activity SDK output before publishing it. It
allows Wine's synthetic drive root, but rejects an unexpected reparse point beneath that root so a
link or mount cannot redirect the generated pack during publication. Wine can expose Linux mount
points—including Btrfs subvolume mounts—as those intermediate reparse points. In that layout,
Sunrise rejects the otherwise valid directory and logs output similar to:

```text
activity_sdk_generation stage=pack ... result=failed
sdk_generation result=fail ... detail="invalid_input"
```

Without `activity_sdk.pack` and its catalog, activity mission seeds cannot be resolved. The game can
reach orbit normally, but selecting a destination remains on the loading screen indefinitely.

The launcher maps the game directory directly to `D:` before starting `D:\destiny2.exe`. The Linux
mount is then represented by the permitted Wine drive root rather than an unexpected component
beneath `Z:`. This preserves Sunrise's existing reparse-point protection; the launcher does not
disable or bypass the validation. The issue depends on the Wine-visible filesystem ancestry and is
not specific to Arch Linux.

The launcher also prevents two copies using the same game directory from running simultaneously.
Sunrise's embedded server uses fixed local ports, so an overlapping process would otherwise fail to
bind and the game would report that it could not connect to the Destiny 2 servers.

The first launch can spend several minutes generating `activity_sdk.pack`. If Sunrise reports a
startup timeout, leave the game open until the Sunrise popup closes, exit normally, and launch it
again.

## Adding the launcher to Steam

The script can be added as a non-Steam game:

1. Select **Games > Add a Non-Steam Game to My Library**.
2. Browse to `scripts/linux/sunrise-run` and add it.
3. Set its launch options to `--game-dir "/path/to/ProjectSunrise"`.
4. Do not force a Steam Play compatibility tool for the shortcut. The script invokes Proton itself.

## Troubleshooting

If Steam or Proton is not discovered, pass their paths explicitly with `--steam-root` and
`--proton`. Steam installed in an unusual location may not be detected automatically.

Steam Flatpak paths are detected on a best-effort basis but have not been tested. Flatpak Steam may
also need filesystem permission for a game directory outside its sandbox.

If the selected Wine drive is already in use, choose another letter:

```bash
./scripts/linux/sunrise-run --game-dir /path/to/ProjectSunrise --drive r
```

Use a new `--prefix` path to test with a clean compatibility prefix without deleting an existing
one.
