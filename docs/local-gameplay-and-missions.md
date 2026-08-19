# Local gameplay and mission scope

## Current scope

Sunrise supports offline destination loading and exploration. It does not currently support most
mission gameplay. The repository README lists missions, enemies, NPCs, quests, and persistent saves
as unsupported features. See the [project README](../README.md).

The local gameplay code is a foundation for server-owned activity state. It does not yet implement
Destiny mission scripts or complete NPC behavior.

## Existing gameplay foundation

The local server has these gameplay subsystems:

- BAP, HTTP, and gameplay transport;
- activity sessions, membership, and entity-slot management;
- peer, group, association, and DTLS handling;
- fixed-step logical worlds and replication;
- actor creation, removal, authority, motion, and teleports;
- generic navigation, combat, damage, triggers, objectives, credits, incidents, and timers;
- world checkpoints and command journals.

The gameplay runtime starts the endpoint and the physics host. See
[`gameplay_runtime.cpp`](../Sunrise/src/server/gameplay/gameplay_runtime.cpp).

The host command model includes `SpawnActorCommand`, combat configuration, trigger, objective, and
timer commands. See
[`host_command.h`](../Sunrise/src/server/gameplay/physics/world/host_command.h).

These commands are generic server primitives. They do not describe an enemy type, an encounter
wave, a campaign objective, or a Destiny mission state machine.

## Default world behavior

A new gameplay world currently uses the scriptless fallback policy. That policy creates no actors,
submits no commands, and makes no mission decisions. See
[`fallback_policy.cpp`](../Sunrise/src/server/gameplay/physics/host/fallback_policy.cpp).

The activity-session bridge opens its world with a null policy. The source comment states that the
fallback spawns nothing and makes no mission choice. It also states that the world bounds are still
a zero box. See
[`physics_session.cpp`](../Sunrise/src/server/gameplay/physics/host/physics_session.cpp).

This is why the presence of actor and combat commands does not mean that the project already runs
campaign encounters.

## Player spawn sets

Sunrise does extract authored spawn-set data from installed packages. The client uses this data to
select a destination, a bubble, and a player spawn-set hash.

The spawn-set builder scans installed packages and publishes stems, hashes, and points. See
[`spawn_set_build.cpp`](../Sunrise/src/client/content/spawn_sets/spawn_set_build.cpp).

This supports player placement in loaded destinations. It is separate from enemy or encounter
spawning.

## Reimplementing a Red War mission

Loading a Red War destination and selecting a player spawn is a bounded extension of the existing
activity and spawn-set work.

Recreating a playable campaign encounter is a major task. It needs all of these parts:

1. A mission policy that selects and changes mission phases.
2. Data extraction or definitions for each encounter, wave, trigger, objective, and checkpoint.
3. Actor creation with the correct transform, faction, authority, and client-visible identity.
4. NPC movement, targeting, abilities, damage, deaths, despawns, and respawns.
5. Client-compatible replication and activity messages for every state transition.
6. Support for mission-specific features such as restricted zones, wipes, dialogue, cinematics,
   rewards, and scripted world changes.
7. Per-mission validation against the supported game build.

The generic host can provide some of the server-side mechanics. It cannot replace the mission logic
or NPC runtime by configuration alone.

## Relative difficulty

| Goal                                                         | Expected scope                                                                                        |
| ------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------- |
| Load a destination and place the player at an authored spawn | Moderate. Sunrise already has destination and spawn-set systems.                                      |
| Create a static test actor through the local host            | Significant but limited. The work needs a policy, actor data, and client-compatible replication.      |
| Show enemies with basic movement and damage                  | Large. The work needs actor presentation, combat behavior, lifecycle handling, and protocol research. |
| Recreate one playable Red War encounter                      | Major reverse-engineering and implementation work.                                                    |
| Recreate the full Red War campaign                           | A long-term project with extensive mission, NPC, and client-compatibility work.                       |

## Development direction

Add new gameplay behavior to the local server path when possible. Do not use a client patch as a
substitute for server-owned mission state. This follows the project contribution guidance in the
[README](../README.md) and the layer boundaries in [AGENTS.md](../AGENTS.md).
