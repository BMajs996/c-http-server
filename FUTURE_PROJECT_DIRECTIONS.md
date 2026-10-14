# Future Project Directions After Phase 17

After completing all 17 phases, we can extend the C gateway or build a separate
networking project using the experience gained from it. These are possibilities
to evaluate later, rather than additional committed roadmap phases.

## Extend the gateway


|Direction                    |What we would build and learn                                                                           |
|-----------------------------|--------------------------------------------------------------------------------------------------------|
|HTTP/2 support               |Multiplexed streams, flow control, header compression, and scheduling                                   |
|HTTP/3 and QUIC              |UDP-based transport, independent streams, congestion control, and connection migration                  |
|Multiple CPU workers         |Scaling across cores, connection ownership, shared state, and coordinated shutdown                      |
|Distributed gateway instances|Coordinated rate limits, configuration, cache invalidation, and backend discovery                       |
|Persistent response cache    |Disk storage, crash recovery, metadata consistency, and bounded asynchronous I/O                        |
|Plugin system                |Routing and policy extensions through a versioned interface, with WebAssembly as an option for isolation|

## Build another C networking project

### 1\. A real-time messaging server

Use the gateway in front of a chat or notification backend written in C. Add
channels, subscriptions, message delivery, persistence, and slow-consumer
handling.

This would turn the gateway into part of a complete application and deepen our
understanding of long-lived connections and delivery behavior.

### 2\. A network traffic analyzer

Capture traffic, reconstruct TCP streams, inspect supported protocols, and
measure latency.

This would deepen our understanding of what happens below HTTP. Protocol
inspection would depend on which traffic is visible; encrypted application data
would require a suitable decryption or endpoint instrumentation setup.

### 3\. A DNS resolver or DNS proxy

Implement caching, expiration, upstream selection, malformed-packet handling,
and UDP/TCP transport.

This would introduce another protocol with different message framing, transport
behavior, and cache semantics.

### 4\. A distributed key-value store

Build a network protocol, persistent storage, replication, and eventually
consensus.

This would introduce distributed systems alongside C networking, with explicit
work on durability, failure recovery, and consistency.

### 5\. A network fault-testing tool

Introduce delay, bandwidth limits, disconnects, and partial transfers between
applications.

We could use this tool to test the gateway and other servers under repeatable
failure conditions.

## Preferred project directions

The preferred standalone projects are:

- **1\. Real-time messaging server**
- **2\. Network traffic analyzer**
- **4\. Distributed key-value store**
- **5\. Network fault-testing tool**

A suggested order for these projects is:

1.  **Network fault-testing tool:** start with a TCP relay that introduces
    controlled delay, bandwidth limits, and disconnects. Use it to exercise the
    gateway and every subsequent server. Add packet-level fault injection later
    if needed.
2.  **Real-time messaging server:** build a small C backend behind the gateway,
    beginning with channels, subscriptions, bounded message queues, and
    explicit slow-consumer behavior. Define delivery guarantees before adding
    persistence or replay.
3.  **Network traffic analyzer:** begin with packet capture and TCP stream
    reconstruction for supported traffic, then add protocol inspection and
    latency analysis. Correlate observations with gateway logs and faults
    introduced by the testing tool; encrypted payloads require endpoint
    instrumentation or an appropriate decryption setup.
4.  **Distributed key-value store:** begin with a single-node protocol and
    durable storage, then add replication and a precisely defined consistency
    model. Use the fault-testing tool to test recovery and partitions.
    Consensus and integration as messaging storage come only after the store's
    durability and failure behavior are established.

These projects can eventually form a connected system: the gateway fronts the
messaging servers, the key-value store provides storage where its guarantees
fit, and the testing and analysis tools help diagnose behavior. Each project
should also remain usable independently.

## Optional gateway extension path

For extending the gateway itself, the previously suggested sequence remains an
option:

1.  **Multiple CPU workers:** improve gateway scalability and define ownership
    across workers.
2.  **HTTP/2 support:** learn a more complex protocol with multiplexing and flow
    control.
3.  **A C real-time messaging backend:** use the gateway in a complete
    application.

The resulting system could look like this:

```text
Clients → C gateway → C messaging servers → persistent storage
```
Choose the next direction based on what the finished project reveals: measured
bottlenecks, the features we enjoyed building, and whether we prefer
infrastructure or application protocols.

## Relationship to the roadmap

[ROADMAP.md](ROADMAP.md) describes the planned work through Phase 17. This
document records options for what could follow; it does not change the current
phase order or imply that any of these extensions are already implemented.
