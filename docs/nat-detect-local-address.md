# The NAT detector's `local_addr`, and who may read the NAT type

Companion to `ARCHITECTURE.md` ("NAT Traversal") and `HowOFFSWorks.md`. This
document records two findings from porting the NAT detection to a downstream
codebase: the detector's `local_addr` field is declared, read by the
classifier, and never assigned — so the `NAT_TYPE_OPEN` verdict is
unreachable in production; and the NAT type is state that only the owning
actor may read, which a port that keeps it on a shared object has to respect.

Date: 2026-10-24. Verified by reading `src/Network/nat_detect.c` and
`src/Network/nat_detect.h` in full, and by searching `src/` for every
assignment to `local_addr` — there is none.

## 1. The field

`src/Network/nat_detect.h:36-37`:

```c
  /* Local address for comparison */
  uint32_t local_addr;
```

`nat_detect_classify()` takes it as its first argument and uses it in three
places (`src/Network/nat_detect.c:24`, `:32`, `:39`), each of them the
`NAT_TYPE_OPEN` test:

```c
  if (local_addr == reflexive_addr_a) {
    return NAT_TYPE_OPEN;
  }
```

and the dispatch hands the detector's own field to it
(`src/Network/nat_detect.c:136`):

```c
        detect->detected_type = nat_detect_classify(
            detect->local_addr,
            detect->reflexive_addr_a, detect->reflexive_port_a, detect->got_response_a,
            detect->reflexive_addr_b, detect->reflexive_port_b, detect->got_response_b);
```

## 2. Why nothing fills it

`nat_detect_start()` stores the two relay hosts and their ports and creates
the two relay clients. The address-response handler fills the two reflexive
slots, the two endpoint ids, and the classification. Nothing writes
`detect->local_addr`, so it stays 0 for the detector's whole life.

With `local_addr == 0`, the OPEN test can only fire if a relay reported the
address 0 — which no relay does, because a report of 0 means "no report".
**The `NAT_TYPE_OPEN` verdict is therefore unreachable**, and
`nat_detect_classify()`'s OPEN branch is live only in a unit test.

## 3. What the OPEN case means

"The relay saw this node at the node's own address" — no NAT rewrote it.
That is the case where the relay and the node are on the same network: the
address the relay reports is the node's internal address, and a hole punch to
it lands on the node directly.

It is also the answer to the open question in `docs/Investigate.md` item 4
("develop a method for nodes inside the same local networks to connect to
each other. Should we include internal ip's with connection information?").
Nothing extra has to be included: **the relay already reports the internal
address** whenever the relay is on the same LAN — the report is the source
address of the packet, private or public. What is missing is the node's own
side of the comparison, so the node cannot recognise that report as its own
address.

## 4. How to fill it

The address the comparison needs is the source of the packets the relay
received, which is the local address of the relay connection. MsQuic hands it
over directly:

```c
  QUIC_ADDR local_addr;
  uint32_t len = sizeof(local_addr);
  MsQuic->GetParam(Connection, QUIC_PARAM_CONN_LOCAL_ADDRESS, &len, &local_addr);
```

(`deps/msquic/src/inc/msquic.h:1032`).

Three details matter:

- **Read it while the connection is live.** The detector asks for it long
  after the connection came up, and a connection closed by then cannot be
  asked. Capture it in the connection's CONNECTED callback and keep it.
- **Compare the address only.** The local port is the socket's ephemeral one;
  the port in the relay's report is the NAT's mapping. The two are never
  equal, so the comparison must be on the address.
- **It must be the address of the path the relay's packets came from.** A
  multi-homed node has several local addresses, and the OPEN verdict is only
  meaningful against the one the relay's packets actually used.

## 5. What it changes, and what it does not

It changes the **label**: a node with no NAT is reported as `NAT_TYPE_OPEN`
rather than as a cone NAT.

It changes **no path**. Every cone type — `OPEN`, `FULL_CONE`,
`RESTRICTED_CONE`, `PORT_RESTRICTED_CONE` — is treated the same by the
decision points: `conn_state_init()` puts all four in
`CONN_STATE_TRYING_DIRECT`, and the direct attempt is gated on
`local_nat_type != NAT_TYPE_SYMMETRIC` alone. Only `NAT_TYPE_SYMMETRIC` is
behaviourally distinct, because a symmetric NAT's mapping depends on the
destination and a hole punch cannot be aimed at it.

## 6. The NAT type's state, and who may read it

The second finding: **upstream keeps the NAT type inside the network
actor's state, and nothing reads it from another thread.**

- `network->local_nat_type` is written in the network actor's own
  dispatch — the default at `src/Network/network.c:359` and the
  detection's result at `:5843` — and every read of it (`:845`, `:1106`,
  `:1734`, `:5916`) is inside that same dispatch. The thread that writes
  it is the thread that reads it.
- `peer->peer_nat_type` is written (`src/Network/conn_state.c:145`,
  `src/Network/peer_connection.c:33`) and never read: the decision points
  consult the peer's `conn_state`, not the type.
- `conn_state_get()` — the accessor that would read a peer's state from
  another thread — has no callers in `src/`.

A port that keeps the NAT type on an object the application holds a
pointer to — rather than inside the actor that owns it — has to make that
field atomic, or the application reads a value another thread is writing.
It is not only a stale value: the compiler may hoist a plain load out of
the loop that waits for the answer, so the wait may never see the update
at all.

## 7. The default, for the same reason

`network->local_nat_type` starts as `NAT_TYPE_PORT_RESTRICTED_CONE`
(`src/Network/network.c:359`), not as `NAT_TYPE_UNKNOWN` — before the
detection has answered, the node assumes a cone NAT, so a direct attempt is
allowed. The peer's own record starts as `NAT_TYPE_UNKNOWN`
(`src/Network/peer_connection.c:33`), because the peer's NAT type is not
known until it says so.

A port that mirrors this keeps the two defaults apart: the node's own type
starts as a cone NAT, a peer's starts as unknown.
