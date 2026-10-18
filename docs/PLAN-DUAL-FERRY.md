# Plan: dual-cell networks (avenue, highway, ground highway) and ferry

Research: `docs/research/dual-ferry-641.md`. Builds on `docs/PLAN-EDGE-TILE.md` and `docs/PLAN-REMOVAL.md`. Status: draft for review.

## Facts the plan rests on

- Avenue (type 8, net 6), elevated highway (3, net 2) and ground highway (12, net 12) make **one connection object per lane**: two objects on adjacent cells just outside the same edge, each with its own marker. Nothing else distinguishes them from singles: same `CreateConnection`, same record entries, same `DeleteConnection`.
- `dir` is cosmetic (rotates the marker 180 degrees) and is not stored; the game recomputes it on reload from pair position, edge and the Drive on Right byte at `0x00b21cc1`.
- On load the game lays the stub from the higher-coordinate lane (hi) only; the width-2 tool widens toward the lower coordinate (lo), so hi's 3-cell line covers lo's row (inferred). It demolishes hi's row only, sets the edge flag on hi's edge cell and on its 0x04 partner (lo's edge cell), and verifies hi's edge cell only.
- Garbage deals look for a connection of type 3, then 12, then 8, then 2, then 10: mirroring duals is what enables garbage deals over avenues and highways.
- Ferry (type 9) is created by each city from its own terrain (edge water runs), never by the player, has no marker and no network tile, and the load path creates a mirrored ferry even from a removal entry.

## Rules

### Pairs

1. Whitelist gains 3, 8, 12 (`IsMirrorableConnectionType`). `ConnectionToNetworkType` maps 3→2, 8→6, 12→12.
2. Each lane keeps its own key (`ConnectionKey` per publisher inner cell), its own `mirroredConnections` pointer, record injection, tombstone and removal, exactly as a single. Nothing in the removal pass changes: `ClearEdgePiece` per lane already clears the 0x04 partner.
3. Pairing, per publisher, per pass, from the snapshot alone: among listed dual entries that convert to our edge, group by (type, edge), sort by the along-edge coordinate, pair greedily from the lowest when the coordinates differ by exactly 1. The lower is lo, the higher is hi. An unpaired lane is created and injected but never laid (vanilla gives it primary=0) and is logged once per key.
4. Create order within a pair: lo, then hi. `dir_lo = (our edge is north or east) ^ !DriveOnRight`, `dir_hi = !dir_lo`, `DriveOnRight` read once per pass from the byte at `0x00b21cc1` through a `src/*641.h` accessor and logged. Singles keep `dir=0`.
5. Lay from the pair, not from a lane: when `layEdgeNetwork` and the pair's hi key has not been tried this session (both lane keys go into `edgeLayTried`), and both lanes have a non-null `FindConnection`, call `LayEdgePair(ctx, type, hi, lo, cells)`:
   - geometry: `EdgeStubGeometry` for each lane; the block is the union (2 by 3 cells);
   - survey all 6 cells with the existing rules (network pieces of the same net are kept, the marker is kept, everything else is demolished; both rows, not hi's only, because a building on lo's row would make the placement fail or lay half);
   - funds before and after, `DemolishRegion` on the block, `Init`;
   - `PlaceNetworkOccupants` over hi's 3-cell line with net 2, 6 or 12 (same call as singles; the dual width comes from the network type);
   - verify both edge cells hold a piece of that net; if lo's edge cell has none, log it and place once over lo's line as well (covers the case where the widening inference is wrong);
   - edge flags: set the side byte on hi's edge cell and on lo's edge cell (log the 0x04 bits so the partner inference is checked, but do not depend on it);
   - second `PlaceNetworkOccupants` over hi's line; verify both edge cells again;
   - on failure: `Shutdown`, delete **both** lanes' connections (vanilla deletes hi only and leaves a dangling lo), log, and leave the keys as tombstones (listed, null here: not re-created until the publisher stops listing them).
6. `LayEdgePiece` for singles is unchanged. Shared steps (survey, demolition, tool guard, flag update, verify) are factored so the pair path does not copy them.
7. The connection-created event `0x8A0D15DA` fires once per lane in the publisher and in us; the existing handler only sets the dirty bit.

### Ferry

8. Type 9 stays off the whitelist. When a publisher lists a type 9 on our edge, log once per key whether `GetConnectionsOfTypeToCity(9, A)` is empty here (a terrain mismatch worth knowing), and do nothing. Reason: our own scan creates a ferry for the same water run at load, and a mirrored one could never be removed.

## What must not break

- Singles: no change in behaviour or log lines.
- Removal of one lane by the publisher (bulldozing half an avenue stub is possible): that lane's key is removed as a single; the other lane stays until it is unlisted. Vanilla's add=0 does the same per lane.
- Rebuild detection and tombstones per lane, unchanged.
- A pair split across a tile corner or whose partner lies on another tile: unpaired, connection only.
- Demolition never touches more than the 2 by 3 block, and never same-net pieces or the marker.

## Accepted divergences (record in Q12)

- We demolish lo's row too and delete both lanes on failure; vanilla does hi's row only and leaves lo's connection dangling.
- An unpaired dual lane gets no stub (vanilla: same).
- Ferry not mirrored (vanilla's load creates it; ours comes from our own terrain scan instead, which is what the game does for the player's own city anyway).

## Experiment (Q12)

Cheats first (`RegionSyncMirror`), then the timed path.

1. A drags an avenue to a bare stretch of the shared edge. A's publish lists two type 8 entries on adjacent cells. B `RegionSyncMirror`: `pair lo=(..) hi=(..) dir_lo=.. dir_hi=.. driveOnRight=1`, two `CreateConnection`, two record injections, `LayEdgePair ... PlaceNetworkOccupants=1` twice, `edge cells lo=1 hi=1`, 2 by 3 avenue stub under two arrows pointing the same way as vanilla's (compare with a vanilla-loaded avenue connection elsewhere in the region), funds unchanged.
2. B can drag an avenue from the stub inward; a garbage deal becomes available over it (type 8 is in the garbage lookup).
3. Elevated highway (3) and ground highway (12): same. For 3, note whether the raised marker and the elevated stub come out right.
4. A house on lo's row: demolished (`DemolishRegion=1`), stub laid.
5. A bulldozes the whole stub: B's next pass removes both lanes (`removed=2`), two `EdgeClear` lines, arrows gone. A bulldozes one lane only: B removes one lane; the other stays until unlisted.
6. A ferry listed by A on B's edge: `MirrorConnections: type 9 (ferry) not mirrored, own ferry to (x,z) present=1`.
7. Save both, reload both: stubs persist, no duplicates, `already laid` for both lanes.
8. Timed path, no cheats: 1 and 5 again.
