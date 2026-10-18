# Plan: remove connections and deals that a live publisher no longer lists (slice 2c)

Research: `docs/research/connections-641.md` (DeleteConnection, `0xEA0D163E`), `docs/research/edge-tile-641.md` (add=0 sequence), `docs/research/deals-641.md` (Cancel, grace, probation, ownership table), liveness (`src/WriterIdentity.cpp`, Q9 step 3). Reviewed 2026-10-18; both blockers (publisher restart, rebuild races) are folded in.

## Rule

A snapshot lists the publisher's complete connection and deal sets every publish. For a publisher whose snapshot is **Alive** at mirror time, whose `cityLoadedAt` is known, and whose lists are known, an entry that this publisher **listed earlier in the same publisher session** and does not list now has been removed by the publisher. We then remove our side.

Publisher session = (writer host, pid, start, `cityLoadedAt`). It is stored per tile. When it changes, or the tile's snapshot is missing or Dead, that tile's seen keys are dropped before the pass: a relaunched or reloaded neighbor converges by creation only (vanilla's rule), never by deletion.

Never removed: on first sighting, with Unknown liveness (other host, `CheckWriterLiveness=0`, schema < 3), with unknown lists, for non-adjacent tiles, for entries the publisher still lists, for Proposed-only deals (`0x1` without `0x2`).

## Connections

Keys are the existing `mirroredConnections` entries (publisher tile, type, publisher inner cell); no second map. The set now also records the local connection pointer returned by `FindConnection` at the last pass.

Removal pass, per publisher, after the create pass: for each key of this publisher not listed now:
1. `pConn = FindConnection(type, bx, bz)`. Null: the player already removed it locally; erase the key, done.
2. If `pConn` differs from the pointer recorded last pass, the player bulldozed and rebuilt it since: refresh the pointer, keep the key, no removal this pass (rebuild race).
3. `DeleteConnection(type, bx, bz)`, then the add=0 edge clear for mapped network types (`ClearEdgePiece`: on the edge cell's `0xC772BF98` occupants with `IsOfType(netType)`, `SetEdgeTypesFlag(netType, f & ~(0xFFu << side*8))`, same on the `0x04` partner cell if the bit is set; private tool `Init`, one-cell `PlaceNetworkOccupants(sx, sz, sx, sz, 0, nullptr, true, -1)` with the return and `GetFailureState` logged, `Shutdown`, `Release`). Ferry and unmapped types: `DeleteConnection` only. Pieces stay (vanilla).
4. Record copy: if the publisher's record copy holds the counterpart (`rc641::FindConnection(pA, type, cell outside its edge)`), `SetIsActive(false)` and, when it is one we injected, erase it from the `+0x90` list (code exists in `EnsureRecordConnection`). Otherwise a later save writes a stale active entry into the publisher's file and its next load rebuilds the stub through demolition.
5. Erase the key.

Tombstones (create side of the race): a key whose local connection is gone (`FindConnection` null) while the publisher still lists it is not re-created; the key stays as a tombstone until the publisher stops listing it, then it is erased. So a player who bulldozes a mirrored connection does not get it back every pull, and the publisher's removal (their side, on their next pull) clears it on both ends.

## Deals

`seenDeals` value becomes {amount, local type}. Removal pass before the existing "forget unlisted" loop: for each seen key of this publisher not listed now (or listed with Cancelled `0x4`): `FindDeals(localType, pA)`; exactly one match: hold the list across the call and `Cancel()` it, log the flags before and after and whether status 2 arrived inside `Cancel`; any other count: log, skip. Then erase the key. Deals listed with only Proposed are ignored by both passes.

## Publisher side

- `CaptureConnections` / `CaptureDeals`: any skipped entry (null other side) sets `*Known = false` and logs at Info, so a partial capture can never read as a removal.
- `connectionsKnown` is published as false until `0x6AC284F3` (connections ready) has arrived this session: the load-time publish runs before vanilla's own reconcile.
- The `0xEA0D163E` and `0xEA418D8F` handlers already set the dirty bit; the next SimNewDay publish carries the change.

## Gating

`SyncRemovals` (INI, default 0 until Q11 is observed; `RegionSyncMirror` always runs removals). Connection removal also needs `SyncConnections`, deal removal `SyncDeals`. Liveness is read at mirror time.

## What must not break

- Player work: no deletion without a prior listing in the same publisher session; rebuilds detected by pointer change; tombstones stop re-creation; no demolition in removal.
- Third tiles: keys are per publisher; connections to other tiles are never touched.
- Grace and probation: a deal the publisher still lists is never cancelled here.
- Re-entrancy: `Cancel` and `DeleteConnection` post or send messages; the handlers only set the dirty bit and the mirror runs under `InPullScope`.

## Accepted divergences (document in Q11)

- A subscriber reloaded from an older save keeps a connection or deal the peer removed earlier (first sighting only records); vanilla would have applied it from the file at load only if the peer had saved.
- Each `DeleteConnection` queues an add=0 entry on the publisher's record copy; a save writes it into the publisher's file (vanilla has the same).
- Bulldozing a house under a pre-2b arrow deletes the connection on both sides (the game's `DeleteOccupantConnection` runs first); that is vanilla's result at the next load too.

## Experiment (Q11)

1. Road mirrored both ways with stubs. B bulldozes its stub: B logs `0xEA0D163E`, publishes without it; A's next pull logs `Removal: connection ... DeleteConnection`, edge flags cleared, one-cell re-solve result, arrow gone, road stays. Reverse. Power line the same (confirm the one-cell re-solve result for poles).
2. Cancel the water deal in A: B logs `Removal: deal ... Cancel()`, status 2, deal gone from B's window, Imported/Exported updated. Reverse. Also a deal with two matches (expect skip).
3. Exit A with the Exit button while B runs: `dead=1`, nothing removed.
4. A quits to the region without saving and reopens the city: B logs the session change and drops A's keys; nothing removed; connections converge by creation.
5. B bulldozes and rebuilds a stub within one month: A must not delete it (pointer change logged).
6. B reloads next to running A: no removal in either.
7. `CheckWriterLiveness=0`: no removal.
8. Flip `SyncRemovals=1`, repeat 1 and 2 with no cheats. Save in both orders, reload the subscriber first each time: nothing resurrects, nothing is demolished.
