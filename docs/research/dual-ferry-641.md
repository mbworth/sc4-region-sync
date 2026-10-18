# Dual-cell networks and ferry connections (SC4 1.1.641)

Static analysis only (Ghidra copy `scratchpad/dual`). Builds on connections-641.md and edge-tile-641.md. "Inferred" marks anything not read instruction by instruction.

## Type values (confirmed)

The vendored `ConnectionType` enum is right. The type tables in the binary agree with it.

| Conn type | Name | Net type (apply `0x4aa6d0` jump table `0x4ab7b8`) | Cells per connection run | Marker |
|---|---|---|---|---|
| 3 | Highway (elevated) | 2 | 2 (pair) | yes, raised 15.5 (`0xab1ca8`) |
| 8 | Avenue | 6 | 2 (pair) | yes |
| 12 | GroundHighway | 12 | 2 (pair) | yes |
| 9 | Ferry | none (default case `0x4aacfb`) | 1 per water run | no |

The network tool's net→conn table `0xaa84c4` (13 entries) maps 0→2, 1→1, 2→3, 3→0, 4→5, 5→4, 6→8, 7→6, 8→7, 9→11, 10→0, 11→10, 12→12. Nothing maps to 9, so ferry never comes from the network tool. Streets (3) and one-way roads (10) make no connection.

## 1. Representation

- **One `cSC4NeighborConnection` per lane cell.** An avenue to the edge makes 2 objects at two adjacent cells just outside the edge, on the same edge. Each object has its own marker prop on its own inner cell.
- **Builder path (A).**
  - `0x628bd0` runs during the drag. For each cell of the final step (the row across the pen axis), it pushes a 0x18-byte record `{key, edgeFlag, x, z, type, dir}` onto tool `+0x10c`.
  - Commit `0x624610` then, for each record:
    1. Deletes connections of the other network types already at that cell (an upgrade, such as road to avenue, drops the road connection).
    2. Calls `CreateConnection(type, x, z, dir)`.
- **`dir` is not stored on the object.**
  - CreateConnection `0x6d4860` writes only these fields: `+0x24` type, `+0x28/+0x2c` cell, `+0x30/+0x34` region cell. `dir` (arg 4) is read only to pick the marker's orientation.
  - The serialized connection has no dir field.
  - `cISC4NeighborConnection` exposes no direction: the header has no slot for it, and the binary has none either.
  - The only place dir survives is the marker prop's orientation. Read it with `cISC4PropOccupant::GetOrientation` on the `0x29F10000` prop at the inner cell.
- **What CreateConnection does with `dir`:**
  1. Creates prop `0x29F10000` through the PropManager (`[0x00b43d10]`, city slot 35): `CreateProp` (slot 15), SetPosition on the AsOccupant result, `SetOrientation(o)` (prop slot 10).
  2. Sets property `0xCAA46595=1`.
  3. Adds the prop with AddCityProp (slot 20, the occupant overload; MSVC reverses overload order).

  Orientation `o` by edge and `dir`:

| Edge (test order in `0x6d4860`) | Marker cell | o (dir=0) | o (dir=1) |
|---|---|---|---|
| west `x<0` | (x+1, z) | 3 | 1 |
| north `z<0` | (x, z+1) | 0 | 2 |
| east `x>=CellCountX` | (x-1, z) | 1 | 3 |
| south (else) | (x, z-1) | 2 | 0 |

That is, `o = (side+3) & 3`, plus 2 when dir=1 (side: W0 N1 E2 S3). `dir` is cosmetic: it rotates the arrow 180 degrees and nothing else reads it.

## 2. Receiving side, `0x4aa6d0`, for a pair

**Sort order** (comparator `0x4a78e0`; dedupe on type,x,z happens first):
- Key 1 is the side code: W=0 (`x<0`), N=1 (`z<0`), E=2 (`x>=CellCountX`), S=3.
- Then x ascending, then z ascending. Type is **not** part of the key.

**Bits** (loop `0x4aa891`):
- A counter `k` increments on every entry with `add=1` and type in {3, 8, 12}, across the whole queue.
- `primary = (k even)`. So the 1st dual entry gets primary=0, the 2nd gets 1, the 3rd gets 0, and so on.
- `dir = primary`. It is inverted if `x>=0 && z<=H` (north or east edge), and inverted again if `byte [0x00b21cc1] == 0`.
- `0x00b21cc1` is the **"Drive on Right"** property `0x49be9565` (bound at `0x513247`, Bool, default true). It is read as a raw byte, and nonzero means right-hand traffic. Nothing else in the exe reads it.

| Entry of the pair | Cell | primary | dir (drive right) | dir (drive left) |
|---|---|---|---|---|
| first = lower coordinate along the edge | lo | 0 | 1 on N/E, 0 on W/S | inverse |
| second = higher coordinate | hi | 1 | 0 on N/E, 1 on W/S | inverse |

**add=1 for the pair, in queue order:**

1. **lo (non-primary).** Tool AddRef. `FindConnection` set: skip. Otherwise `CreateConnection(type, lo, dir_lo)` only: no demolition, no Init, no lay. Then Shutdown/Release.
2. **hi (primary).**
   1. Builds the 3-cell region of **hi's row only**, from hi's inner edge cell 3 cells inward (same table as edge-tile-641.md). It skips cells whose traffic-map entry is nonzero.
   2. `DemolishRegion` with layer 1.
   3. `Init`.
   4. `CreateConnection(type, hi, dir_hi)`.
   5. `PlaceNetworkOccupants(hi.sx, hi.sz, hi.ex, hi.ez, 0, null, true, -1)` with net 2, 6 or 12.
   6. Verify a piece of that net on hi's edge cell. If there is none: `DeleteConnection(hi)` only (lo's connection stays, a vanilla quirk).
   7. Edge flags: on hi's edge cell, `SetEdgeTypesFlag(net, (f & ~(0xFF<<side*8)) | (2<<side*8))`.
   8. Then the **partner step**. Take the 0x04 bits of the last matching piece, test in priority order byte0 x-1, byte1 z-1, byte2 x+1, byte3 z+1, take the first set one, and apply the same set on that one cell. For a well-formed pair this is lo's edge cell.
   9. The second `PlaceNetworkOccupants` over the same 3-cell line.
   10. Shutdown/Release.

**Why hi lays the line:** the second lane of a width-2 drag goes to the lower coordinate. In `ComputeDraggedCells` `0x639790`, the widening offset is `1-w` (that is, -1) when the end direction is 1 or 3 on an x-major drag, or 1 or 7 on a z-major drag. All four edge lines (W end dir 3, E 1, N 7, S 1) widen toward z-1 or x-1, so the line from hi covers lo's row. This is inferred to hold for `PlaceNetworkOccupants`, which shares the cell code. Vanilla never demolishes lo's row.

**add=0 for a pair:** there is no primary test, so each lane runs the single-cell sequence:
1. `FindConnection`, then `DeleteConnection(lane)`.
2. Clear the side byte on that lane's edge-cell pieces and on its 0x04 partner (the other lane).
3. `Init`, one-cell `PlaceNetworkOccupants(lane, lane)`, `Shutdown`.

The partner's flags are therefore cleared twice. That is harmless.

**Vanilla parity quirk:** an unpaired dual entry anywhere in the queue (a lane whose partner went to another tile, or a pair split across an edge corner) shifts primary for every later dual entry.

## 3. Symmetry A to B

- **Cells.** Lanes map one to one through the shared along-edge coordinate (`ToSubscriberFrame`). A's lower lane is B's lower lane. A's edge flips (E to W, N to S), so `NE_B = !NE_A`.
- **A's dir** (from `0x628bd0`):
  - `base = (cell x == CellCountX || cell z == -1)` (east or north). It is inverted when the service `0x441e5070` slot 38(0) slot 25 returns false; that this is the drive side is inferred.
  - The first cell of the final step gets `base`, the other gets `!base`.
- **B's dir:** `dir_B(lane) = primary(lane) ^ NE_B ^ !DR`, with primary 0 for lo and 1 for hi. If A's step lists lo first (ascending, inferred from the min/max widening), then `dir_B = !dir_A` for each lane.
- **Rule: do not publish dir.** B computes it from pair position and its own edge, exactly as the load path does. dir only rotates the marker, and the vanilla reload of B would recompute it the same way.

## 4. Ferry (type 9)

**Created by terrain, not by the player.**
- `0x73d850` scans the four edges for runs of water cells, splitting each run every 64 cells (one per neighbor tile edge).
- It deletes type-9 connections on cells that fail its check (slot 8).
- For a run longer than 2 cells with no type-9 connection in it, it calls `CreateConnection(9, outside cell near the deepest point, dir=0)`. The cell convention is the same as other types: just outside the edge. Captures (85,-1) and (95,128) match it.
- It runs on PostCityInit `0x26D31EC1` (if the city is established), on CityEstablished `0x26D31EC4` (DoMessage `0x740160`), and from `0x747c76` (unidentified, probably a terrain change).
- So **each city derives its own ferry connections** from its own terrain.

**How CreateConnection differs for ferry:**
- The `type==9` jump at `0x6d4941` skips the marker and its props.
- It still pushes the connection, computes the region cell, queues ChangeSymmetricConnection on the other record, and posts `0x8A0D15DA`.
- It does no water or cell checks of its own.

**Load path (`0x4aad4b`):** for type 9 the switch reaches the default case **before** the add test. It calls `CreateConnection(9, x, z, dir bit=0)` for both add=1 and add=0. So a mirrored ferry is never removed by the game.

**Users:**
- `FindConnectionSuitableForDeal` does not use ferry. The deal-type mapping in `0x6d1fc0`:
  - `0x4773FFE0/1` → 4 (power)
  - `0x4773FFE2/3` → 5 (pipe)
  - `0x4773FFE4/5` → the first non-empty of 3, 12, 8, 2, else 10
- That last row means **garbage deals use highway, ground-highway and avenue connections**.
- Ferry is read through `GetConnectionsOfType(9)` by traffic (`0x720f00`, `0x723620`) and by `0x5710a0`.

**Counterpart:** B's own scan normally makes one type-9 connection per water run at its own deepest cell. That can be a different cell from A's. The scan reuses any existing type-9 connection inside the run, and it does not delete duplicates.

## 5. Validating a pair in B

All of these can be checked from the snapshot plus B's geometry:

| Check | Rule |
|---|---|
| Type | both in {3, 8, 12} and equal |
| Same edge | both convert (ToSubscriberFrame) to an outside cell with the same fixed coordinate (-1 or `cells`) |
| Adjacent | the along-edge coordinates differ by exactly 1 |
| Exclusive | each cell belongs to one pair. Pair greedily from the lowest coordinate per edge, as vanilla's sort does |
| Same publisher and target tile | already enforced by the `otherTile` filter |

An unpaired lane (its partner is on another tile, or the pair is malformed): vanilla would give it primary=0, meaning connection only, no lay. Do the same and log it.

## Recommended mirroring rules

**Pairs (3, 8, 12):**
1. Validate and pair (section 5). Order within a pair: lo, then hi.
2. `DR = byte [0x00b21cc1]` (log it). `dir_lo = NE_B ^ !DR` and `dir_hi = !dir_lo`.
3. `CreateConnection(type, lo, dir_lo)`, then `CreateConnection(type, hi, dir_hi)`. Inject a record copy for each lane as for singles.
4. Lay from hi only, using `LayEdgePiece` with net 2, 6 or 12. Changes needed:
   - (a) Check both rows are clear, or demolish the 2x3 block. This deviates from vanilla, which demolishes hi's row only and would fail or partly lay when lo's row is built.
   - (b) After setting hi's side byte, apply the same set on the 0x04 partner cell (expect lo's edge cell; log a mismatch).
   - (c) Verify both edge cells hold a piece. On failure delete both connections (vanilla deletes hi only).
5. Removal: run the existing single `ClearEdgePiece` per lane. It already handles the partner step.
6. First run from a cheat, avenue first. Log the marker orientation of each lane next to A's.

**Ferry (9):** do not mirror; keep it off the whitelist. B's own scan already creates its own, and the game never removes mirrored ones. At most, log when A lists a ferry for B's edge and B has no type-9 connection within the same run (a terrain mismatch). If ever needed: `CreateConnection(9, bx, bz, false)` only, with no lay and no removal.

## Anchors

| What | Address |
|---|---|
| CreateConnection marker orientation and props | `0x6d494a`-`0x6d4b15` |
| Builder edge records / commit | `0x628bd0` / `0x624610` (tool `+0x10c`, stride 0x18) |
| Apply dir/primary loop; ferry default case | `0x4aa891`-`0x4aa9a8`; `0x4aacfb` |
| Queue comparator | `0x4a78e0` |
| Widening across pen axis | `0x639790` (offset `1-w` or `w-1` from end dir `+0x80`) |
| Partner step in add=1 | `0x4ab25f`-`0x4ab299` |
| Ferry edge scan | `0x73d850` (callers `0x740160`, `0x73eba0`) |
| Deal type to connection type | `0x6d1fc0` (nsim slot 23) |
| Drive on Right binding | `0x513247` (`0x49be9565` → `0x00b21cc1`) |
