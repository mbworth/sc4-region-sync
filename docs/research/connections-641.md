# Neighbor connections in SC4 1.1.641: how they propagate, and how to replay one live

Static analysis only (Ghidra, copy at `connections/ghidra`). Nothing has been run in the game yet. The web search turned up no prior art: Null45's 150 repos and nam-dll only read connections (`sc4-neighbor-connection-logging` calls `GetAllConnections` and the getters). Nobody calls Create/Delete/ChangeSymmetric, and nobody uses either message ID.

## Anchors (facts)

| Object | Address / value |
|---|---|
| cSC4NeighborsSimulator main vtable | `0x00ab1ee0` (ctor `0x006d09d0`), msg target `0x00ab1ed0`, serializable `0x00ab1eb8`; IID `0x0773FEF0`; global `DAT_00b43d8c`; city field `+0x188` |
| Neighbors sim slots | Header order matches the binary: 5 RequestCreateConnection `0x6d15e0`, 6 CreateConnection `0x6d4860`, 7 DeleteConnection `0x6d4640`, 10 HasConnection, 11 FindConnection `0x6cf6b0`, 16 GetAllConnections, 20 GetAdjacentCities `0x6d01e0`, Init `0x6d38f0`, DoMessage `0x6d42b0`, Write/Read `0x6cfb60`/`0x6cfdc0` |
| cSC4NeighborConnection | vtable `0x00ab2034` (ctor `0x6d4680`, 0x44 bytes), IID `0x0773FEF2`. Fields: `+0x24` type, `+0x28/+0x2c` connection cell x,z, `+0x30/+0x34` region cell on the other side, `+0x38` bit0 visited / bit1 inactive, `+0x3c` list. Header slots match. |
| cSC4RegionalCity connection slots | 20 FindConnection(type,x,z) `0x4a7b40` (returns `cISC4NeighborConnection*`; the header's `cISC4RegionalCity*` is wrong), 21 GetAllConnections `0x4a8db0`, 22 ChangeSymmetricConnection(conn,bool) `0x4a8df0`, 24 UpdateCityCache(city) `0x4ad290`. Serializable Write `0x4a7d10`, Read `0x4ac5b0`; DoMessage `0x4abd40`. |
| Regional record fields | `+0x90` std::list head of `cISC4NeighborConnection*` (this city's own connections). `+0x94/+0x98/+0x9c` = vector begin/end/cap of 16-byte **pending symmetric changes** `{u32 type; i32 x; i32 z; u8 add; pad[3]}`. Cells are in the record owner's local frame, on the cell just outside its edge (-1 or size). `+0xa0` byte = resync flag. |

## 1. Where connections live

- **The city's .sc4, neighbors sim subfile.** It holds version 2, a count, then each serialized connection (version 1, type, x, z, rx, rz, flags byte, property holder) (`0x6cfb60`, `0x6cf080`).
- **The city's own regional record (region view, clsid `0x6A037C24`).** Write `0x4a7d10` saves the `+0x90` connection list (each connection serialized) and also the `+0x94` pending vector. UpdateCityCache(city) touches `+0x90` (`0x4ad345`), which suggests it copies the simulator's list into the record before a save. That reading is plausible but I have not traced it.
- **ChangeSymmetricConnection(conn, add)** on record R:
  1. Calls `conn->GetConnectionCell` and `GetType`.
  2. Converts the cell with `0x6ce920`, which uses the app's current regional city as the source frame and finds the tile on the other side.
  3. Appends `{type, x', z', add}` to R's pending vector. Nothing else happens.

  Its only callers are CreateConnection (`add=1`) and the delete helper `0x6d17b0` (`add=0`), and both call it on the **other side's** record (`conn->GetCityOnOtherSide()`).

## 2. City A builds to its edge

The network code, when a drag reaches the edge, calls `RequestCreateConnection`. Despite the name, that function only shows the "connect to <city>?" dialog and returns the answer. It does not create anything, and its bool is unused. Creation goes through CreateConnection(type, x, z, dir):

1. If HasConnection, return.
2. Allocate the connection, push it onto the sim list `+0x1c`, set the fields, and compute rx, rz (`0x6ce820`).
3. `conn->GetCityOnOtherSide()->ChangeSymmetricConnection(conn, 1)`. In A's process this writes a pending entry onto **B's in-memory record**.
4. If the type is not Ferry, place a marker occupant `0x29F10000` on the inner edge cell. Its orientation comes from the edge and `dir`, and it is elevated for types 3, 7 and 11. Its properties are `0xCAA46595=1`, plus `0xCAA45670/71/72` for Pipe and Subway. It is inserted through the occupant manager.
5. Post `0x8A0D15DA` with data1=type, data2=x, data3=z (async MessagePost).

The **return value is not set** (EAX is left from the last Release). Treat the return as garbage and use FindConnection afterwards.

**B at load:**
1. Neighbors sim Init `0x6d38f0` calls `cISC4Region::ReloadCity` (slot 15, `+0x3c`) on every adjacent tile. That re-reads A's record from A's .sc4, connection list included.
2. Later, message `0xCA63D517` arrives. It is posted by `0x74a390` after a view-ready counter reaches 2; its identity is unconfirmed. B's record DoMessage `0x4abd40` then runs:
   - `0x4a9580`, only if the `+0xa0` flag is set: queue deletes for B's own connections.
   - `0x4a9220`: for each adjacent record from GetAdjacentCities(list, true), read its `+0x90` list; for every connection whose other side is B, mirror the cell (`0x6ceba0`) and queue `{type,x,z,1}`.
   - **`0x4aa6d0` (apply)**:
     - Deduplicates and sorts the queue.
     - Maps connection type to network type (2→Road 0, 1→Rail 1, 3→Hwy 2, 5→Pipe 4, 4→Power 5, 8→Ave 6, 6→Subway 7, 7→LRT 8, 11→Mono 9, 10→Dirt 11, 12→GndHwy 12).
     - For `add=1` with no connection yet: builds the network piece at B's edge through the network tool (`0x4aae96`), then calls `CreateConnection(type,x,z,dir)` (`0x4aaf8b`). Ferry goes straight to CreateConnection (`0x4aad4b`).
     - For `add=0` with a connection present: removes the edge network occupants.
     - Clears the vector and posts `0x6AC284F3`.

So **yes, B's edge stub is built from the neighbors' records at load**, not from B's own save. A does not need to write B's file; whether it ever does is unknown.

## 3. Live in running city B

- `RequestCreateConnection` is UI only. Do not use it.
- `CreateConnection(type, bx, bz, dir)` on B's simulator, called mid-game, should create the connection object and the marker occupant, and post `0x8A0D15DA`. That message is handled by the demand sim (`0x6a9470`, the same code path as deletion) and by `0x7241b0` (vtable `0x00ab33c8`, probably traffic). It does **not** lay the edge network tile. It also queues a stray pending entry on A's record copy in B (harmless unless B writes A's record).
- **`dir` (the 4th bool):** it flips the marker 180°. In `0x4aa6d0` it is a per-entry bit that alternates across the two cells of dual networks (types 3, 8, 12) and is inverted by the global `0x00b21cc1` (likely drive side). It is therefore the travel direction of that half of a dual carriageway. For single networks it is probably 0, but that is unverified.
- **Deals:** `FindConnectionSuitableForDeal` and `ProposeDeal` read the simulator's list, so they should see a connection made by CreateConnection. Whether power or water actually flows without the edge network tile is unknown.

**Recommended sequence** (it replays the load path, so the stub and connection match what vanilla builds):
1. Mirror A's snapshot connection into B's frame: `bcell = (A.pos*64 + A.innerCell) - B.pos*64`. That is the cell just outside B's edge (-1 or size on one axis).
2. Push `{type, bx, bz, 1}` into **B's own record** vector at `+0x94` using the game allocator (`0x90cf54` new, `0x90cf63` delete). Our CRT must never free or realloc that buffer. Alternatively, write in place when `end < cap`.
3. Call `0x4aa6d0` as a thiscall with ECX = B's regional record. Do it from a cheat, with try/catch, logging before and after with HasConnection and GetAllConnections.
4. For deletes, push `add=0`. Deletion also posts `0xEA0D163E`.

**Fallback experiment** (cheaper, header-only): call `CreateConnection` and check whether a marker and a deal appear. Then compare against step 3.

**Do not** call ChangeSymmetricConnection on B's own record with A's data: it converts from the current city's frame, so the coordinates come out wrong.

## 4. Message IDs (facts)

- `0x8A0D15DA`: connection created (posted by CreateConnection `0x6d4b3c`).
- `0xEA0D163E`: connection deleted (posted by `0x6d17b0`, which DeleteConnection, DeleteOccupantConnection and DeleteAllOfType all reach).
- Both carry data1=type, data2=x, data3=z in the posting city's frame. A's plugin can subscribe to both and publish immediately.
- **Correction for OPEN-QUESTIONS.md Q2:** `0xEA0D163E` is "deleted", not "neighbor list changed". The demand sim recomputes on create too.
- `0x6AC284F3` is posted after an apply pass.

## 5. Minimal snapshot per connection

`type` (u32), `cellX`, `cellZ` (A frame, from GetConnectionCell), `innerX`, `innerZ` (GetConnectionInnerCell), `regionCellX`, `regionCellZ` (GetRegionCellOnOtherSideOfConnection, which serves as a cross-check), `otherTileX`, `otherTileZ` (the target tile's position, so B can filter to itself), and `active` (GetIsActive).

Plus, once per snapshot: A's tile position and size. Publish the full set each time and let B diff it against `GetConnectionsToCity(A)`, so creates and deletes need no separate log. `dir` is not needed if B goes through `0x4aa6d0`; it recomputes it.

## GetAdjacentCities crash

- The SC4List layout matches the game's: a single root pointer, with nodes laid out `{next, prev, value}`.
- Elements are raw `cISC4RegionalCity*`, not slots. Each one is **AddRef'd** (`0x6d01e0`), so the caller must Release each one. The vendored `SC4List` destructor does that (verified 2026-10-17, see deals-641.md ownership table).
- The function clears the output list first (`0x411df0`).
- The cause of the earlier crash is not found statically. It could be from treating elements as slots, or from the clear running on a list that was not empty.

## Unknowns and risks

- What `0xCA63D517` is, and what `0x4a8570` does. I have not decoded either.
- Whether A ever writes B's `.sc4`.
- Whether the bitsets in `0x4aa6d0` start at zero, and so whether `dir` is 0 for single networks.
- Whether `0x4aa6d0` is safe outside its normal message context. It uses the city globals at `0x00b43cec`, `0x00b43d0c` and `0x00b43d8c`.
- Calling `0x4aa6d0` makes us depend on a non-virtual address. That is acceptable only because we are locked to 1.1.641.
- Inserting into a game-owned vector is the riskiest step.
- **Two-way loop:** B's CreateConnection queues an entry on A's record in B. If A also replays B's connections, both sides must dedupe against HasConnection, the way the game does.
