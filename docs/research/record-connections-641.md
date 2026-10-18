# Neighbor record +0x90 connections (SC4 1.1.641), static analysis

## 1. FindConnectionOnOtherSide
- Slot 13 `0x6cdfa0` (conn): `t = c->GetType()`, `(x,z) = c->GetConnectionCell()` (B frame), then tail-calls slot 14 `0x6cede0` (type,x,z). Overloads are in reverse order, as MSVC lays them out.
- `0x6cede0` calls `0x6ce920(x,z,..,&rec,&x',&z')`. That function picks the edge from the **current city's** cell counts (city `0x174`/`0x178`): x<0 west, z<0 north, x>=w east, z>=h south. It adds the current regional city's GetPosition/GetCitySize to get the neighbor tile, then gets `rec` with `Region->GetCity(tx,tz)`. It returns the cell just outside **rec's** edge, in rec's frame. B's (128,55) becomes A's (-1,55). Then `rec->FindConnection(t,x',z')` (RC slot 20).
- RC slot 20 `0x4a7b40` walks rec+0x90 and matches `GetType()==t && GetConnectionCell()==(x',z')`, i.e. fields +0x24/+0x28/+0x2c. It does not check flags, the inner cell or the region-cell fields.
- `0x6d1fc0` checks, by deal type:
  - **Buy** (E0/E2): B's own connections of the type to A (slot 18), and for each one the other side must be non-null and `GetIsActive()`.
  - **Sell** (E1/E3): only B's **own** connection's `GetIsActive()`. No other-side lookup.
  - Garbage: no activity check.

## 2. Who fills +0x90
- **Owner's process:** UpdateCityCache(city) `0x4ad290` at `0x4ad345` calls `city->GetNeighborsSim()->GetAllConnections(rec+0x90)` (sim slot 16 `0x6cff00`: clear, then copy with AddRef). It runs from the record's DoMessage `0x4abd40` on `0x26c63343` (sent by the App save routine `0x455e20` just before writing) and on `0x26d31ec1`, own record only.
- **Neighbor's process:** only Read `0x4ac5b0`. It allocates 0x44 bytes with `0x5e55e0`, runs ctor `0x6d4680`, push_backs a GZ-allocator node with AddRef, and deserializes. It is reached through region load / ReloadCity (loader `0x5dc070`, TGI `0xCA027EDB/0xCA027EE1`). `0x4a9220` and `0x4aa6d0` (the `0xCA63D517` path) only **read** neighbors' +0x90 and write the own pending vector and sim. Shutdown `0x4a8470` clears it via `0x411d70`, which Releases each element and frees the node through the `DAT_00b207dc` allocator.
- Correction: `0x4a86f0` is not a push helper. It is the range insert behind GetAllConnections (thiscall, dest list in ECX, `(where, first, last, tag)`, AddRefs each element).

So in B, A's record has only what was in A.sc4 when B loaded. Most likely A's power line postdates A's last save, so (-1,55) is absent, or it was saved inactive.

## 3. Injecting into A's record in B
- Factory: `0x6d4b90` (new 0x44 + ctor) is registered at `0x460A6D` with clsid **`0x0A134F24`** (`0x6cdff0`). So `CreateInstance(0x0A134F24, 0x0773FEF2)` works. QI `0x6ce000` answers 1, `0x0773FEF2` and `0xE4FDA3D4` with `this`. The ctor zeroes everything, so flags +0x38 = 0, i.e. active.
- No setters exist for type or cell. Write them raw: +0x24 type, +0x28/+0x2c the connection cell in **A's frame**.
- `Repair(pA)` (slot 10 `0x6cf250`) recomputes +0x30/+0x34 (the other side's region **tile**) from the cell using pA's position and size. That is public and correct.
- GetConnectionInnerCell (`0x6cf1b0`) uses the current city, so it is wrong for an injected entry. The deal path does not use it.
- No public path exists:
  - ChangeSymmetricConnection only appends to the pending vector.
  - `pA->UpdateCityCache(Bcity)` would overwrite A's record with B's data.

**List layout (checked 2026-10-17): root pointer only, no size field.** The +0x90 object is one pointer to a sentinel node `{next, prev, value}`, as in the vendored `SC4List`. None of the game's list code writes `[list+4]`:
- Read `0x4ac5b0` is the `cIGZSerializable` method (vtable `0xa8faf8`, stored at record+8 by ctor `0x4ab7f0`), so its `this+0x88` is record+0x90. Per entry it loads the root (`MOV EAX,[EBX+0x88]`), allocates a 0xc node, stores the value with AddRef, and links it before the root: `node->next=root; node->prev=root->prev; root->prev->next=node; root->prev=node` (`0x4aca15`..`0x4aca23`). It then Releases the ctor's reference, so the list owns one.
- Range insert `0x4a86f0` (`where` is a node) does the same link-in per element with AddRef; nothing else is written.
- Clear `0x411d70` (ECX = list object) Releases and frees every node, then sets `root->next = root->prev = root`. No count is reset.
- Record+0x94 is not a list size: Read uses it (`this+0x8c`..`+0x94`) as begin/end/capacity of the 16-byte pending vector.

Recommended sequence (cheat, try/catch, Info logs):
1. `pA = FindRegionalCity(A)`. Log every entry of `pA->GetAllConnections`.
2. Dedupe: `e = (cISC4NeighborConnection*)pA->FindConnection(4,-1,55)`. If found, `SetIsActive(true)` if needed and skip to step 7.
3. `CreateInstance(0x0A134F24, 0x0773FEF2, &c)`.
4. Raw-write +0x24=4, +0x28=-1, +0x2c=55 (from A's snapshot `GetConnectionCell`).
5. `c->Repair(pA)`. Check `c->GetCityOnOtherSideOfConnection() == App->GetRegionalCity()`.
6. `reinterpret_cast<SC4List<cISC4NeighborConnection*>*>((uint8_t*)pA+0x90)->push_back(c)`, then `AddRef` once for the list (the game's inserts AddRef; vendored SC4List does not). SC4List's node allocator is the same GZ allocator service.
7. Verify: `sim->FindConnectionOnOtherSide(bConn)` returns c and `FindConnectionSuitableForDeal(0x4773FFE0,pA)` is non-null. Then ProposeDeal.

## 4. GetIsActive
- slot 13 `0x6d4760` returns `!(flags>>1 & 1)`. The ctor zeroes the flags and CreateConnection does not touch them, so a new connection is active. The flags are serialized.
- Setters: the power sim `0x6ee960` (on SimNewDay `0x66956814` and SimIdle `0x6695681A`, from DoMessage `0x6efb70`) iterates **its own city's** type-4 connections, clamps each cell inside the city, and calls `SetIsActive(powerSim slot 37(x,z))`, probably IsPowered. The water sim `0x6dd380` does the same for type 5 (grid bit 0).
- So B's mirrored connection at (128,55) goes **inactive** on the next pass unless (127,55) is powered. That kills B-side sell deals and FindUnvisited-type uses. It does not affect buy-side FindConnectionSuitableForDeal, and injected A-side objects are never re-evaluated in B.

## 5. Persistence: yes, it leaks
`0x455e20` (App save, vtable `0xa86b2c`), after writing its own record, calls `GetAdjacentCities(list,true)`. For each neighbor with a save path it runs `0x5dc2e0`, which opens **that neighbor's .sc4** read-write and writes the in-memory record (`0x5dc1a0`, the same TGI). So B's save writes B's copy of A's record, including injected +0x90 entries, pending vector, deals and stats, into A.sc4.
- Vanilla uses this to deliver symmetric changes. It also answers "does A ever write B's .sc4": yes, on every save.
- Consequences:
  - Injected entries must be exact and deduped, or they persist as duplicates.
  - Each instance's save clobbers its neighbors' region records with its own stale view, until that neighbor saves again. This matters for Q4/autosave.

## Unknowns
- Power sim slot 37 identity (IsPowered vs IsWired), and whether `0x6ee960` touches connections every call.
- Whether the buy-side deal UpdateState also needs B's own connection to be active.
- Not run in game. Whether A's record in B actually lacks (-1,55) or holds it inactive is unconfirmed; step 1's log settles it.
