# Plan: remove connections and deals that a live publisher no longer lists (slice 2c)

Research: `docs/research/connections-641.md` (DeleteConnection, `0xEA0D163E`), `docs/research/edge-tile-641.md` (add=0 sequence), `docs/research/deals-641.md` (Cancel, grace, probation), liveness (`src/WriterIdentity.cpp`, Q9 step 3).

## Rule

A snapshot lists the publisher's complete connection and deal sets every publish (schema 2+). So for a publisher whose snapshot is **Alive** (same host, process checked) and whose lists are known, an entry missing from the snapshot means the publisher removed it. Dead snapshots are already deleted before mirroring; Unknown liveness (other host, schema < 3) never triggers removal.

Scope of what we remove on our side:

- **Connections**: a connection of ours to that publisher, at a cell adjacent to the publisher's tile, whose mirrored key (publisher tile, type, publisher inner cell) was listed in an earlier snapshot of this session and is not listed now. Vanilla does the same at load (add=0 pending entries from the neighbor's record): `DeleteConnection(type, x, z)`, clear the side byte of the edge flags on the edge cell's pieces of that network type, `Init`, one-cell `PlaceNetworkOccupants(sx, sz, sx, sz, 0, nullptr, true, -1)` (re-solve as a dead end), `Shutdown`, `Release`. The pieces stay, as in vanilla.
- **Deals**: a local deal with that publisher (commodity key) that `seenDeals` holds and the snapshot no longer lists (or lists with the Cancelled flag): `Cancel()` on the local deal, then forget the key. The game's own status run removes it.

"Listed in an earlier snapshot of this session" is the guard against first-sighting removals: on the first snapshot from a publisher we only record what it lists (`seenConnections`, `seenDeals`). Removal needs a previous listing. This keeps a city that loads next to a running neighbor from deleting its own vanilla connections just because the neighbor's running state differs; those converge through creation, not deletion.

## Symmetry

- A built the road, B mirrored it. B bulldozes its edge piece: B's `DeleteOccupantConnection` drops B's connection, B's snapshot stops listing it, A's seen set has it, A deletes its connection object and re-solves its edge cell (vanilla's add=0 result; A's road stays as a dead end). A's snapshot then stops listing it too; B had already removed it.
- A cancels the deal: A's snapshot drops it (or lists it Cancelled), B cancels its copy. Same the other way.
- Player removes a mirrored connection in B but A keeps its side: with this rule A also drops its connection object (as vanilla does on A's next load). If that turns out unwanted, the alternative is to re-create in B each pull, which fights the player; the vanilla-faithful choice is removal.

## Code placement

- `NeighborMirror`: `seenConnections` map per publisher (key as today, value = last listed), `seenDeals` already exists. After the create and follow passes, a removal pass per publisher: for each seen key not listed now, run the removal, then erase. Both passes skip when `!snapshot.connectionsKnown` / `!dealsKnown`, and when `ctx.publisherAlive == false` (new `MirrorContext` field set by the director from `WriterLiveness(snapshot) == Alive`).
- `EdgeNetwork`: `ClearEdgePiece(ctx, type, bx, bz, cells)` implementing the add=0 sequence; `MirrorConnections` calls it after `DeleteConnection`.
- Settings: `SyncRemovals` (INI, default 0 until Q11 is observed; `RegionSyncMirror` always runs removals).
- Director: no new messages. The `0xEA0D163E` and `0xEA418D8F` handlers already set the dirty bit so the publisher's next SimNewDay publish carries the change.

## What must not break

- Never remove on a first sighting, on Unknown liveness, on a snapshot with unknown lists, or for a tile that is not adjacent.
- Never touch connections to third tiles or connections the publisher still lists.
- Deals: `Cancel()` only on deals whose other city is the publisher; never on deals in grace or probation that the publisher still lists.
- No demolition in removal (vanilla leaves the pieces).

## Experiment (Q11)

1. Both instances, road mirrored both ways (stub laid). B bulldozes its edge stub. Expect in B: `0xEA0D163E`, publish without the connection. In A after its next pull: `Removal: connection type=2 ... DeleteConnection`, edge flags cleared, one-cell re-solve logged, arrow gone, road stays. Reverse.
2. Cancel the water deal in A. Expect in B: `Removal: deal 0x... Cancel()`, status 2 event, deal gone from B's window, Imported/Exported updated. Reverse.
3. Exit A with the Exit button while B runs: B must remove nothing (dead snapshot discarded, logged as `dead=1`).
4. Flip `SyncRemovals=1`, repeat 1 and 2 with no cheats. Save both sequentially, reload, confirm nothing comes back.
