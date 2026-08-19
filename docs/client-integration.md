# Client integration

## Purpose

Sunrise does not use only a custom local server. It also changes selected Destiny 2 client behavior
inside the game process.

Sunrise builds as `steam_api64.dll`. The game loads this DLL in place of its expected Steam API
library. The exported entry points are in
[`Sunrise/src/dllmain.cpp`](../Sunrise/src/dllmain.cpp).

## Integration layers

Sunrise uses these layers together:

1. The Steam shim provides the Steam API entry points that the game calls.
2. The client runtime scans the main game image for known function signatures.
3. The hook runtime detours selected game and Steam functions.
4. The local server processes intercepted HTTP and BAP requests.
5. The client hooks change checks that would reject the offline environment.

The client runtime resolves executable targets before it installs the game hooks. A target signature
must match the supported game build. If a required signature does not match, Sunrise stops client
activation. See
[`client_hook_activation.cpp`](../Sunrise/src/client/runtime/client_hook_activation.cpp).

## Network and SignOn integration

The network hook group replaces these client paths:

- network transport selection;
- HTTP request execution;
- bubble-authority decoding;
- untracked-content lookup;
- SignOn readiness and failure handling;
- Steam networking authentication status;
- Steam networking certificate handling.

The hook specifications are in
[`network_hook_entries.cpp`](../Sunrise/src/client/hooks/network/lifecycle/network_hook_entries.cpp).
The lifecycle code installs the game hook group with content and investment hooks as one unit. See
[`network_hook_lifecycle.cpp`](../Sunrise/src/client/hooks/network/lifecycle/network_hook_lifecycle.cpp).

The transport hook changes the SDR transport selection to direct sockets. The certificate and
availability hooks keep the native calls but accept the local networking state. See
[`platform.cpp`](../Sunrise/src/client/hooks/network/platform.cpp).

The local Server registers handlers for intercepted HTTP and BAP traffic. It also starts local BAP
transport and gameplay services. See
[`server_runtime.cpp`](../Sunrise/src/server/runtime/server_runtime.cpp).

Sunrise can also point the client to a separate server. This mode changes the host in SignOn URLs.
It is not the default embedded-server path. See
[`external_server_route.cpp`](../Sunrise/src/client/hooks/external_server/external_server_route.cpp).

## Client boot and activity admission

The game needs several client states before it can load an activity. Sunrise installs separate hooks
for character selection, profile setup, activity composition, orbit handoff, join readiness, owner
activity slots, region state, world steps, spawn holding, and fade release.

The hook group is in
[`bootflow_hook_lifecycle.cpp`](../Sunrise/src/client/hooks/bootflow/bootflow_hook_lifecycle.cpp).

In this code, `spawn` usually means player admission into an activity. It does not mean that Sunrise
creates enemy encounters. For example, the spawn-gate probe reads client participation, lifetime,
team, world, and slice-set state before it reports why the player cannot spawn. See
[`spawn_gate_probe.cpp`](../Sunrise/src/client/hooks/bootflow/spawn/spawn_gate_probe.cpp).

## Package and content integration

Sunrise reads package data from the game installation at runtime. It builds local catalogs for
scenarios, spawn sets, items, and other content. It does not store game data in this repository.

The package-trust hook changes selected native trust results so the target build can load its local
package data. It keeps the ordinary structural checks and changes only the trust gates that block
this offline path. See
[`package_trust_bypass.cpp`](../Sunrise/src/client/hooks/package_trust/package_trust_bypass.cpp).

## Other client hooks

Sunrise also installs hooks that are not network routes:

- graphics, cursor, and input hooks for the user interface;
- movement hooks for fly, noclip, and teleport;
- infinite-ammo and inactivity hooks;
- diagnostic hooks for retail logs and assertions;
- queue, bitmap, and configuration hooks.

For example, the fly hook writes the local player's velocity before the physics step. It can also
restore vertical position after the step to keep a hover. See
[`fly.cpp`](../Sunrise/src/client/hooks/fly/fly.cpp).

## Result

Sunrise is a combined client-and-server integration. The local services provide protocol responses
and gameplay state. The in-process hooks make the older game client accept and use those services.
Neither part is sufficient on its own.
