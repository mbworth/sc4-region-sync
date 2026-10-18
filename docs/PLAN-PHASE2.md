# Phase 2 plan: connections and deals between running instances

Status: reviewed 2026-10-17 (correctness, safety, simplicity lenses); slice 1 approved for implementation. Evidence: `docs/research/connections-641.md`, `docs/research/deals-641.md` (Ghidra only until the experiments below run).

## Goal, slice 1 (create only)

Player builds a single-cell network (road, rail, power line, pipe, dirt road, monorail, light rail, subway) to the edge in city A; city B, running in another instance, gets the connection within a sim day of the next pull without reloading. Player proposes a deal in A; B's copy appears in B's deal list and budget. Removal, amount follow, dual-cell networks (highway, avenue, ground highway), ferry, and the raw replay of the load routine are later slices, each with its own plan.

## What must not break

- Phase 1 sync (records, demand delta feed) keeps working.
- No writes to `.sc4` files or the Regions folder; only game calls on in-memory objects.
- No new game call runs automatically before it has run from a cheat with its log read.
- No exception reaches game code; no game call from inside a synchronous message handler.
- The plugin never removes or cancels anything in slice 1.

## Facts the plan rests on

Connections:
- `CreateConnection(type, x, z, dir)` on the city's neighbors simulator creates the connection object, places the marker occupant on the inner edge cell, appends a pending entry on the other side's record, posts `0x8A0D15DA`. Delete posts `0xEA0D163E`. Both are posted, so their handlers run on the main thread outside the creating call. `RequestCreateConnection` is only the confirmation dialog. `CreateConnection`'s return value is garbage; confirm with `FindConnection`.
- At load, B's record handler reads the pending entries that A left on B's record and the routine at `0x4aa6d0` lays the edge tile and calls `CreateConnection`. B's own save does not carry A's new connection.
- `dir` alternates across the two cells of dual networks; for single networks 0 is assumed and must be checked in the cheat log.
- The demand simulator re-sums regional population on `0x8A0D15DA` and `0xEA0D163E` (Q2).

Deals:
- Deal types: power `0x4773FFE0/E1`, water `E2/E3`, garbage `E4/E5`; even = buy, odd = sell; B's view of A's deal is `typeA ^ 1`. Flags: 1 proposed, 2 active, 4 cancelled, 8 no connection, 0x10 can't afford, 0x20 no landfill, 0x40 renewal due, 0x80 silent, 0x100 grace, 0x200 probation.
- Each side holds its own copy from its own point of view. B learns of A's deal only at load in vanilla; there is no offer prompt in B.
- `ProposeDeal(type, otherRecord, &out)` returns false without a matching connection in B's simulator list. `AddDeal` then `SetAmount` makes it active and recomputes cost. `SetAmount` sends `0xEA418D8F` synchronously; the others post it. `MarkDealsDirty` only re-pushes own deals onto records. Imported/Exported stats are recomputed from deals.
- `FindDeals`/`FindDealsAll` return deals even though the header says connections.
- `ReloadCity` replaces record objects; never cache a record pointer across pulls.

## Design

### Snapshot schema 2

Additive keys. Counts are explicit; a reader that does not find `connectionCount`/`dealCount` treats the lists as unknown (not empty).

```
connectionCount=N
connection.i.type=2              # ConnectionType enum, whitelist 1,2,4,5,6,7,10,11 in slice 1
connection.i.innerX= innerZ=     # inner edge cell, publisher's frame
connection.i.cellX= cellZ=       # GetConnectionCell (just outside the edge), publisher's frame; absent in older files
connection.i.otherTileX= otherTileZ=   # GetCityOnOtherSideOfConnection()->GetPosition()
dealCount=M
deal.i.type=0x4773FFE4           # publisher's view
deal.i.otherTileX= otherTileZ=   # GetCityOnOtherSideOfConnection()->GetPosition()
deal.i.amount=
deal.i.flags=                    # without 0x80
deal.i.expiresIn=                # days: expiration - current
```

Identity: connection = (type, innerX, innerZ) in the publisher's frame; deal = (commodity, tile pair).

Frame conversion lives in `NeighborSnapshot.h` as pure arithmetic and is covered by the host test: `bx = (A.regionX*64 + innerX) - B.regionX*64`, same for z. Valid when exactly one axis is -1 or `B.cells` (`B.cells = 64 << B.sizeEnum`, to be confirmed against `GetCitySize` in the cheat log) and the other axis is inside `[0, B.cells-1]`.

### Publisher

- Capture moves into the bridge: `CaptureConnections` (own simulator `GetAllConnections`) and `CaptureDeals` (own simulator `FindDealsAll` through a local wrapper that fixes the element type; it is the live list, `GetLocalDeals` can lag). The Dump cheat prints from the snapshot instead of its own loops.
- Publish on the month tick as today. The handlers for `0x8A0D15DA`, `0xEA0D163E` and `0xEA418D8F` (reasons 1..7) only set a dirty bit. `SimNewDay` (`0x66956814`, to be confirmed in the log) flushes it with a normal publish. Nothing else happens in those handlers.
- `0x8A0D15DA`/`0xEA0D163E` also mark the demand baseline for re-take on the next SimNewDay (`lastSum = SumOtherTiles` per entry), since the game re-sums on those messages. The cheat run logs supply before and after to confirm.

### Subscriber, connections (city B, on each pull, after record apply)

For each snapshot connection whose `otherTile` is B's tile and whose type is whitelisted:
1. Convert to B's frame; invalid results are logged and skipped.
2. Identity already in the session set `mirroredConnections`: skip. The set stops resurrection when the player bulldozes a mirror; the cost is that the mirror returns only after restart.
3. `HasConnection(type, bx, bz)`: true skips step 4 (and stops the echo: B's own publish lists the mirror, A finds it present).
4. `CreateConnection(type, bx, bz, 0)`, then `FindConnection(type, bx, bz)` (also for HasConnection=1); log both, Info.
5. Record side (`docs/research/record-connections-641.md`), also when B already had the connection: A's record copy in B holds only what A.sc4 had when B loaded, and the buy-side deal check looks there.
   - Injection (the `GetClassObject` bullet) only from the `RegionSyncMirror` cheat (`MirrorContext::allowRecordInjection`). The automatic SimNewDay path only finds and `SetIsActive`.
   - `pA = FindRegionalCity(A)` fresh. Cheat path, once per pull: log its connections (up to 16) and check the raw `+0x90` list: root non-null, `root->next->prev == root`, `root->prev->next == root`, at most 256 nodes, same element pointers in the same order as `GetAllConnections`. Any failure: Error, no injection.
   - The connection cell must lie just outside A's edge (`-1` or `cellsA = 64 << A.sizeEnum` on one axis, from the region location list), the other axis equal to the inner cell's and inside `[0, cellsA-1]`.
   - `rc641::FindConnection(pA, type, cellX, cellZ)` (RC slot 20 walks `pA+0x90`, matching type and connection cell only, in A's frame). Found: `SetIsActive(true)` if inactive, done.
   - Else `GetClassObject(0x0A134F24, 0x0773FEF2)` (factory `0x6d4b90`), raw-write type `+0x24` and cell `+0x28/+0x2c`, `Repair(pA)` (fills the other-side tile `+0x30/+0x34`). `GetCityOnOtherSideOfConnection()` must be our record, else skip. The object is held in a `cRZAutoRefCount`; after the insert at the end of `pA+0x90` it is AddRef'd once for the list.
   - Verify: `FindConnection` must return the new object, else erase it from the list (which Releases the list's reference) and log Error. Then `FindConnectionOnOtherSide(bConn)`; Info.
   - B's own `GetAllConnections` count is logged before and after this step, to catch a feedback through the `0xCA63D517` path.
   - B's save writes B's copy of A's record into A.sc4 (`0x455e20`), injected entries included, so dedupe by `FindConnection` is mandatory.
6. Record the identity in the set either way. The Mirror summary line counts created and injected connections. A skipped injection (automatic path, failed list check, wrong other side) also records the identity, so it is not retried this session.

### Subscriber, deals (city B, on each pull, after connections)

For each snapshot deal whose `otherTile` is B's tile and whose type is one of the six, with `typeB = typeA ^ 1`:
1. Identity in `mirroredDeals`: skip.
2. `FindDeals(typeB, pA, list)` with `pA` resolved by `FindRegionalCity` on this pull: a match means done; record identity.
3. Log `FindConnectionSuitableForDeal(typeB, pA)` (null explains a failing proposal). `ProposeDeal(typeB, pA, &d)` into a `cRZAutoRefCount`; false is logged and skipped (no connection on B's side yet).
4. While silent: current and start = today (clone the sim date, Release the clone), expiration = today + `expiresIn` via `+=`.
5. `AddDeal(d)`, then `d->SetAmount(amount)`. Record identity. Never hold a `FindDeals` list across these calls.
6. Pull sets `inPull`; the dirty bit set by the synchronous status message is flushed on SimNewDay, never inside the pull.

### Validation

All snapshot values are untrusted: type whitelists, cell bounds against B's size, `amount` in `[0, 10^7]`, `expiresIn` in `[0, 36500]`, `flags` masked to the known bits. Anything else is logged and skipped.

### Settings

- `SyncConnections=1`, `SyncDeals=1` (default since 2026-10-18, after the Q9 cheat runs): automatic mirroring: a pull queues the snapshots, the next SimNewDay mirrors them, including the record-copy injection.
- `RegionSyncMirror` cheat: runs the subscriber steps once for both lists regardless of the settings. This is the experiment.
- `ApplyFields` already skips imported/exported (deal-derived); unchanged.

### Code placement

- `NeighborSnapshot.{h,cpp}`: schema 2 structs, read/write, frame conversion; `tests/snapshot_roundtrip.cpp` extended.
- `RegionalCityBridge.cpp`: `CaptureConnections`, `CaptureDeals`.
- `RegionalCity641.h`: record `FindConnection` with the corrected return type, the `+0x90` list, the connection class/IID and raw field offsets.
- `NeighborsSim641.h`: `FindDeals` wrapper with the corrected element type. Nothing else.
- `NeighborMirror.{h,cpp}`: `MirrorConnections`, `MirrorDeals`, the identity sets, validation. Game-facing like the bridge, no message system.
- `RegionSyncDllDirector.cpp`: subscriptions, dirty bit, SimNewDay flush, `inPull`, cheat dispatch. Each new `DoMessage` branch wrapped in try/catch.

## Experiments (Q9 in OPEN-QUESTIONS)

Before any of them: back up the region folder on the Windows box, `AutoSaveEveryMonths=0`, and nobody saves in either instance until experiment 4 says it is safe.

1. Both instances up, A = TuggersVille, B = BumperVille. In A build a road to the shared edge. A's log: connection created, dirty bit, publish on SimNewDay. In B type `RegionSyncMirror`. B's log: converted cell, size in cells, HasConnection, CreateConnection, FindConnection, demand supply before/after. Look at B's edge: arrow, stub, both, neither. Then in A propose a power deal across that connection; in B `RegionSyncMirror` again; B's log: ProposeDeal result, dates, AddDeal, SetAmount, deal flags. Check B's budget line and, a month later, `amountUsed` > 0 (that is the test of whether power flows without an edge tile; the deal's existence alone proves nothing).
2. Reverse direction.
3. A quits without saving while B runs: B must change nothing (no removal in slice 1, but confirm the dirty-bit and publish paths do not misbehave on a missing snapshot).
4. Flip `SyncConnections=1`, `SyncDeals=1`, repeat 1 with no cheats. Then save both, reload both normally, compare deal and connection lists before and after for duplicates. This result decides whether Q4 needs a save lock before slice 2.

## Deferred, with the reason

- Amount follow: done 2026-10-18 (slice 2a). Per publisher deal the mirror keeps the last published amount and follows only when it changes; first sighting records only; a deal gone locally after being seen is not re-created; keys are dropped when the publisher stops listing the deal. Known limit: concurrent edits of one deal within a month.
- Removal of deals and connections: the review found ways it cancels or re-creates the player's own work (missing snapshot read as empty, resurrection races). Needs tombstones plus a writer liveness check (Q9 step 3: the Exit button leaves a snapshot behind); separate plan.
- Writer liveness: done 2026-10-18 (schema 3). Snapshots carry host, pid and process start; a pull deletes one whose writer on this machine is gone. Not yet observed in-game. Removal above still needs tombstones.
- Raw replay of `0x4aa6d0` for the edge tile: cosmetic only (Q9: a pipe laid under the mirrored arrow carries water).
- Dual-cell networks and ferry: `dir` must be derived from the pair.
- Third-city deals (A with C) on B's copy of A's record: nothing public writes that list.

## Risks

- `0x8A0D15DA` arrives before or after the game's own demand re-sum relative to our handler; the baseline re-take on SimNewDay assumes after. The cheat log decides.
- Both sides persist mirrored copies when they save; vanilla `Init` may or may not dedupe. Experiment 4.
- Everything is pinned to 1.1.641 addresses, as before.
