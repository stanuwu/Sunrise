# Server architecture

This document describes the design and subsystems of the Sunrise local server.

## Server subsystems overview

The Sunrise server runs inside the game process and coordinates three major subsystems:

```mermaid
graph TD
    Server["Sunrise Local Server"]
    HTTP["HTTP Server<br/>(SignOn, Dynamic Manifests, Web APIs)"]
    BAP["BAP Router<br/>(Encrypted Dispatcher, Transactions, Pushes)"]
    Gameplay["Gameplay Server<br/>(DTLS Host, Peer Transport, World Simulation)"]

    Server --> HTTP
    Server --> BAP
    Server --> Gameplay
```

---

## 1. HTTP server

The embedded HTTP server intercepts and handles REST and SignOn traffic.

- **Request dispatcher**: Matches incoming HTTP URLs and method verbs.
- **SignOn handler**: Authenticates client sessions and encodes binary configuration payloads.
- **Content delivery routes**: Supplies dynamic manifest endpoints and fallback web responses.

---

## 2. BAP router and transaction pipeline

The BAP router handles state synchronization, client commands, and push notifications over TCP.

### Request routing

1. Incoming frames decrypt through the authenticated AES-GCM layer.
2. The router extracts the service ID and passes the payload to the registered service handler.
3. Handlers generate binary response payloads.
4. The router encrypts the response and transmits it to the client socket.

### Transaction staging and commits

State mutations (such as dismantling items or changing subclass nodes) execute through a two-phase commit pipeline:

```mermaid
sequenceDiagram
    autonumber
    actor Client as Destiny 2 Client
    participant BAP as BAP Router
    participant State as State Layer

    Client->>BAP: Mutating request (for example, opcode 402 / 403 / 801 / 903)
    BAP->>State: Prepare mutation (Preconditions & snapshots)
    BAP->>BAP: Stage response payload
    BAP->>State: Commit mutation under lock
    BAP->>Client: Send response (Service 11)
    BAP->>Client: Push Queuez frame (Service 123)
```

### Push notification staging

The server maintains push notification queues for connected clients:

- **Activity arrivals**: Notifies the client when session members enter an activity.
- **Membership updates**: Synchronizes character rosters and fireteam slots.
- **Queuez updates**: Broadcasts modified state family objects.
- **Snapshot storage**: Retains latest state revisions for reconnecting clients.

---

## 3. Gameplay server and world simulation

The gameplay server simulates the interactive world, synchronizes peers, and processes player physics.

### Network transport layers

- **Association host**: Executes SRP authentication and issues session encryption keys.
- **DTLS host**: Encrypts and decrypts real-time UDP datagrams.
- **Peer transport**: Tracks sequence numbers, acknowledges received packets, and retransmits lost fragments.
- **Group host**: Coordinates fireteam membership, session parameters, and migration notices.

### Physics and world simulation

Gameplay endpoint processing drains and routes datagrams separately from the fixed-step world
service loop. Inside `HostTickProcessor`, one service tick runs in this order:

```mermaid
flowchart TD
    A["1. Synchronize actor bodies"] --> B["2. Run controller navigation"]
    B --> C["3. Run physics and collect pose commits"]
    C --> D["4. Run combat"]
    D --> E["5. Evaluate triggers"]
    E --> F["6. Apply objective credit"]
    F --> G["7. Run timers"]
```

### World simulation components

- **`BubbleHost`**: Composes the built-in world services and owns the active world slots.
- **`WorldRunner`**: Executes one world's activity policy, commands, actors, and fixed ticks.
- **`ActorControllerService`**: Updates actor positions, health values, and state flags.
- **`MotionValidator`**: Enforces velocity thresholds and prevents illegal client position leaps.
- **`InterestManager`**: Filters entity replication so clients receive updates only for entities in their active bubble.
- **`HostCommand` and `CommandJournal`**: Represent host actions and persist committed command records.
- **`ScriptlessPolicy`**: Provides inert fallback behavior when no mission policy is active.
