# Plan: lay the edge network piece for mirrored connections (slice 2b)

Research: `docs/research/edge-tile-641.md`. Today a mirrored connection is an arrow marker only; the player must lay the piece under it (which works, Q9). The game's load path lays a 3-cell stub from the edge inward with `cISC4NetworkTool::PlaceNetworkOccupants` and marks the edge-facing side with `SetEdgeTypesFlag`. This slice reproduces that for single networks.

## Scope

- Types: road 2, rail 1, pipe 5, power 4, subway 6, light rail 7, monorail 11, dirt road 10 (the whitelist already mirrored). Duals (avenue, highway, ground highway) and ferry stay arrow-only.
- Runs right after a successful `CreateConnection` in `MirrorConnections`, on both the timed path and `RegionSyncMirror`, behind `LayEdgeNetwork` (INI, default 0 until observed; `RegionSyncMirror` always lays).
- Removal: none (no removal in this phase).

## Sequence per new connection (single network)

1. `nm = pCity->GetNetworkManager()`, `tool = nm->GetNetworkTool(netType, true)` (private instance, not the player's), `AddRef`. Null: log, skip.
2. Cells: edge cell `(sx,sz)` = the connection's inner cell; `(ex,ez)` = two cells inward along the axis normal to the edge (table in the research note).
3. Occupancy check: for each of the 3 cells, `occMgr->GetFirstOccupantByStandardCityCell(o, cx, cz, 0xC772BF98)`; power also checks pole `0x2890D4DE` and wire `0x0990B2EA`. Any hit: log `EdgeTile: cell (x,z) occupied, not laid` and skip. No demolition in this slice (the game demolishes with a layer filter; we never destroy the player's work).
4. `tool->Init()`.
5. `tool->PlaceNetworkOccupants(sx, sz, ex, ez, 0, nullptr, true, (eNetworkType)-1)`; log the return and `GetFailureState()` / `GetFailureReasonString()`.
6. Verify: `occMgr->GetOccupantsByStandardCityCells(list, cell, cell, 0xC772BF98, -1)` (power: `0x2890D4DE`); for network occupants, QI `cISC4NetworkOccupant` and `IsOfType(netType)`. No piece: log `EdgeTile: no piece on the edge cell`, leave the connection in place (differs from the game, which deletes it; the arrow-only state is what we have today), `Shutdown`, `Release`, done.
7. Edge flags: for each matching occupant on the edge cell, `f = GetEdgeTypesFlag(netType)`, `SetEdgeTypesFlag(netType, (f & ~(0xFF << side*8)) | (2 << side*8))`. If `f & 0x04040404`, apply the same to the partner cell (research 12d).
8. `PlaceNetworkOccupants` again with the same arguments. Log the return.
9. `tool->Shutdown()`, `tool->Release()`.
10. Power only, before step 5: `zoneMgr->PlaceZone(region, 0, true, true, false, true, false, nullptr, nullptr, 0)` dezones the 3 cells. First slice: skip it and let the occupancy check cover zoned-but-empty cells; log if the cell is zoned.

All inside the existing mirror try/catch; every game call logged at Info before and after (first exercise of these methods).

## What must not break

- The player's active network tool: private tool instance only; never `Shutdown` the shared one.
- Player work: no demolition, no dezoning in this slice; skip when occupied.
- Budget: code placement is free (research, OpenSC4). Log the city funds before and after the first run to confirm.
- Existing connections: step 3 of the research (`FindConnection` skip) is already what `MirrorConnections` does.
- `NeighborSnapshot` stays game-free: no change. Cell math goes next to `ToSubscriberFrame` only if it needs no game types.

## Code placement

- `src/EdgeNetwork.{h,cpp}` (new): `LayEdgePiece(ctx, type, innerX, innerZ, cellsB) -> EdgeResult {laid, reason}`; takes `cISC4City*` through `MirrorContext` (add `pCity`). Occupant filter class not needed (no demolition).
- `MirrorConnections` calls it after `CreateConnection` + `FindConnection` succeed; `ConnectionMirrorResult` gains `laid`.
- `SyncSettings`: `LayEdgeNetwork`.

## Experiment (Q10 in OPEN-QUESTIONS)

1. Both instances, A builds a road to the edge; B `RegionSyncMirror`. Expect: `PlaceNetworkOccupants=1` twice, a road stub of 3 cells at B's edge under the arrow, funds unchanged, B's own tool still works afterwards. Then power line, pipe, rail.
2. Occupied edge cell (a house): expect `occupied, not laid`, arrow only.
3. Save B, reload: the stub persists and the game does not lay a second one (it skips entries with an existing connection).
4. Flip `LayEdgeNetwork=1`, repeat 1 with no cheats.

## Deferred

- Duals and ferry; demolition with the layer filter; removal (with the add=0 sequence); power dezoning.
