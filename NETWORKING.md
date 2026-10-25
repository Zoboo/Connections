# Asynchronous Multiplayer Networking

## Fundamental rule

If networking stops responding, the application must remain responsive. Network latency may delay a connection, discovery result, or remote state update; it must not delay input, gameplay, rendering, animations, or audio.

Keep platform services and transport implementations separate from gameplay and session rules. Reuse one session protocol across transports.

## Transport terminology

This document uses the following product-facing terms. They describe how users find and connect to peers, not necessarily different physical radios.

| Mode | Connection method | User experience |
| --- | --- | --- |
| LOCAL WIRELESS | Nearby device wireless, or discovery and communication over a shared LAN | Discover available sessions and join without entering an address. |
| WIFI | Direct connection to a specified IP address and port | Host specifies a listening port; client enters the host address and port. |

LOCAL WIRELESS can have two distinct implementations:

- **Nearby device wireless:** A platform-specific local communication service connects devices directly. On Nintendo Switch, libnx LDN is a possible backend, subject to service availability and application permissions.
- **LAN:** Devices discover sessions on a shared local network, then connect through ordinary sockets. Discovery can use an appropriate broadcast or multicast mechanism; it does not imply that those packets can cross routers or client isolation boundaries.

WIFI uses an explicit endpoint and does not require discovery. Direct IP connections can also operate over wired Ethernet or a LAN. The label does not imply Internet reachability: routing, firewall rules, and NAT still determine whether an endpoint is reachable.

Nearby wireless, LAN discovery, and direct IP networking should each remain replaceable behind a transport abstraction. Supporting one backend does not automatically provide the others.

## Thread ownership

The main thread owns input, gameplay state, UI, and renderer resources. Audio processing owns its playback state. Neither performs network I/O or waits for remote information.

Networking workers own:

- Platform network initialization and shutdown.
- Discovery and advertising.
- Socket creation, connection attempts, reads, writes, and closure.
- Packet framing and validation.
- Connection timeouts, heartbeats, and disconnect detection.

Workers never modify UI or gameplay objects directly. Deliver validated events or immutable snapshots through a narrow thread-safe interface.

```text
Main/game thread -> bounded command queue -> network worker -> transport
Main/game thread <- bounded event queue   <- network worker <- transport
```

Use bounded queues, fixed-size buffers, atomics, or short critical sections. Never hold a game-facing mutex during network I/O, discovery, image decoding, filesystem access, or waits. Renderer texture creation and destruction stay on the renderer's owning thread.

## Public interface

A transport interface can expose:

```text
startHost(options)      -> enqueue request
startClient(options)    -> enqueue request
startDiscovery(options)-> enqueue request
send(message)          -> enqueue bounded outbound message
requestStop()          -> enqueue cancellation
drainEvents(limit)     -> consume already received events without blocking
```

Connection success and failure arrive asynchronously. A successful enqueue means only that the worker accepted a command; it does not mean the connection or send completed.

Keep socket handles, platform service objects, and transport-specific errors inside the networking layer. Translate failures into session events suitable for presentation.

## Worker scheduling and backpressure

Use blocking socket readiness waits, OS events, or timed waits. Do not busy-poll. Wake workers for outbound commands when practical; otherwise use a bounded wait that also allows timely cancellation and deadlines.

Run discovery and advertising periodically in the background. Never repeat them for every rendered frame.

Define configurable limits for peers, payload lengths, send buffers, receive buffers, and queues. When capacity is exhausted:

- Coalesce replaceable state updates, keeping the newest value.
- Preserve reliable lifecycle and match events using bounded retry or a clear failure path.
- Reject new work or disconnect a persistently overloaded peer rather than allocating without limit.
- Send large, bounded assets in chunks and schedule them behind essential session traffic.

Keep queue locks short. Copy commands or events while locked, then process them after unlocking. Do not log every packet or synchronously write files on critical paths.

## Applying incoming state

Networking is event-driven. Do not perform socket reads or discovery from the frame loop. The main thread may consume a bounded number of already queued events at safe update points, or check a published revision at a modest cadence. This is queue consumption, not network I/O.

Apply changes only when new information arrives. Until then, render the last known remote state and continue local interaction. Bound event consumption so a burst cannot monopolize a frame.

Cache expensive presentation data. Decode received assets outside critical rendering paths, publish completed buffers, and create textures on the renderer thread only when an asset changes.

## Session authority

For games that do not require synchronized simulation, use a host-authoritative session:

- Host owns membership, settings, start, accepted results, and termination.
- Clients report local actions or completion events.
- Host validates identity, session, sequence, and allowed state before accepting an event.
- Host broadcasts authoritative changes.

Do not introduce lockstep or continuous simulation synchronization unless the gameplay requires it. Local actions that can proceed safely should not wait for a server response.

Use stable session player IDs independently of display names. Duplicate names are valid. Platform account identifiers should remain local unless sharing them is explicitly necessary.

## Protocol and reliability

Serialize fields explicitly. Never send raw language structs with compiler-dependent padding, byte order, pointers, or boolean representation.

Include a protocol version, message type, payload length, and relevant session or match ID. Add monotonic sequence numbers where duplicate or stale events matter.

Validate before applying:

- Header and payload lengths, message types, and protocol compatibility.
- Numeric ranges and maximum string or asset sizes.
- Sender identity, ownership, and session membership.
- Whether the message is allowed in the current state.
- Sequence numbers and legal state transitions.

TCP provides ordered byte delivery, not message boundaries. Implement framing, partial reads, partial writes, and bounded buffering. TCP does not prove that the application accepted an event; add application acknowledgements when that distinction matters.

UDP requires explicit reliability where needed: acknowledgements, bounded retries, duplicate suppression, and reassembly limits. Loss-tolerant state can use latest-value semantics. Avoid unlimited retries and retransmission queues.

Typical traffic consists of join/leave, settings, ready/start, result updates, match end, and infrequent heartbeats. Do not transmit unchanged UI, timers, or scoreboards every frame.

## Timing and disconnects

Use monotonic clocks for elapsed time and deadlines. Never compare unrelated device wall clocks. A future start countdown can reduce startup jitter without requiring deterministic frame synchronization.

Match-relative client elapsed times are useful but depend on start alignment and trusted measurements. Host validation can reject implausible values; it cannot independently prove a client's clock or honest execution.

Timeouts and heartbeat handling belong to workers. A stalled peer must not freeze a lobby or active game. Remove disconnected lobby members; during play, mark them disconnected and continue where rules permit. If the host disappears, end the session cleanly unless host migration is explicitly implemented.

Use an explicit state machine, for example:

```text
Inactive -> Discovering/Connecting -> Lobby -> Preparing -> Starting
         -> Playing -> Finalizing -> Results
Any active state -> Disconnected/Error -> Inactive
```

Cancel outstanding operations when leaving a session. Request shutdown without blocking the UI; let the worker release its resources and signal completion. Join only once completion is known during normal interaction. Final application shutdown may join workers, but their I/O must have bounded waits or cancellation so shutdown cannot hang indefinitely.

## Validation checklist

Validate protocol logic separately from platform transports where practical. Then use hardware for platform-specific discovery, permissions, and wireless behavior.

- Join, leave, multiple peers, full sessions, and incompatible versions.
- Invalid endpoints, connection failure, cancellation, and timeout.
- Partial messages, malformed packets, duplicates, and stale sequences.
- Host loss, client loss, and repeated session entry/exit.
- Queue saturation, slow readers, and bounded memory usage.
- Artificial latency, packet loss, and a worker stalled for several seconds.
- Responsive input, stable frame times, animations, and audio during network faults.
- Clean resource release and bounded worker shutdown.

Measure main-thread frame times separately from networking worker load. A network stall should affect only remote-dependent actions; it should not introduce recurring frame overruns or audio underruns.
