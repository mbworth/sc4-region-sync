# Neighbor deal offers: how the game proposes a deal (SC4 1.1.641)

Ghidra, 2026-10-18. Scratch project copy under the session scratchpad (`offer/ghidra`).

## No timer

`cSC4NeighborsSimulator::DoMessage` (`0x6d42b0`) never proposes deals. Offers come only from `ProposeDeals(list, addToSim)` (slot 25, `0x6d21a0`, worker `0x6d0520`), called by:

- the utility advisor `cSC4AdviceNeghborDeal` (class `0x6a3848db`, vtable `0xa83b10`): `0x421ab0` calls `ProposeDeals(list, false)` on each advisor refresh (slot 40, `0x4220a0`) and on `kMessageType_UpdateSelf 0xca8f3db6` after a deal status message `0xEA418D8F`. It turns the result into Lua globals (`g_nd_can_sell_water`, `g_nd_can_buy_water`, `g_nd_count_*`, `g_nd_connection_present_*`); the "X wants to buy" text and any extra gating live in the Lua advice scripts, not the exe.
- the Neighbor Deals window (`0x77e600`, mode 2 of `0x7876b0`): `ProposeDeals(list, true)` appends the proposals to the sim (`sim+0x20`).

`FindConnectionSuitableForDeal` has no other callers (ProposeDeals, ProposeDeal, UpdateState). The vendored header declares the list element as `cISC4NeighborConnection*`; it holds `cISC4NeighborDeal*`.

## ProposeDeals conditions

Prologue: `sim+0xC` must be set (set on ConnectionsReady `0x6AC284F3`, `0x6d45c0`); `UpdateCommodityStats(city)` refreshes the own record; output list cleared. Per-commodity table {buy, sell, commodity, minAmt}; minAmt from the tuning exemplar (slot 37): power `0xea46932c`, water `0x6a469337`, garbage `0x8a46933d`, default 0. Also `0xea4ae9ef` minimum budget (default 0) and `0xca46931f` cost multiplier (default 1.0).

Loop over `GetAdjacentCities(list, false)` (established neighbors only). Sell branch (own type E1/E3/E5 to neighbor A):

1. `FindConnectionSuitableForDeal(type, A)` non-null: own connection of the matching network type to A, `GetIsActive()`.
2. `FindDeals(type, A)` empty.
3. `Y = GetDealMaxAmount(type, A)` (slot 36, `0x6d3620`) `>= minAmt`; `Z = GetDealCost(type, min(minAmt, Y), A)` (slot 35, `0x6d3250`).
4. `X` = A's budget (record slot 50, `+0x170`): `X > minBudget`, `X > Z`, `X > mult * Z`.
5. `0x6d0520`: ProposeDeal plus a random term of 1 to 5 years (`0xaa46934b` / `0xea46934f`).

Buy branch: same, with `X` = own budget and the other side's connection (in A's record copy) must be active. No random chance, no cooldown, no check of the edge network tile, the `0x29F10000` marker, or, for sell, the record-copy connection.

## GetDealMaxAmount

Record commodity node (map at record `+0x180`, lookup `0x4a9b40`): Produced `+0x0` (slot 58), Demanded `+0x8` (slot 60), Exported `+0x10` (slot 54), Imported `+0x18` (slot 56), CostPerUnit `+0x20` (slot 62). Budget slot 50, Established slot 43. The vendored header's slot numbers are 3 lower than the binary's for these.

With `e` = existing deal amount (0 if none), `f` = `0xea4ad2ff` (default 1.0), `m` = minAmt:

- buyerNeed = f * max(D_b + Ex_b, m/f + 1) - (P_b + Im_b - e)
- sellerAvail = (P_s + Im_s) - (D_s + Ex_s - e)
- result = max(0, min(sellerAvail, buyerNeed))

For water, `UpdateCommodityStats` writes Produced from water sim slot 14 (`+0x110`) and Demanded from slot 23 (`+0x130`); water sim vtable `0xab2228`.

**Header bug:** `GetDealCost` and `GetDealMaxAmount` return a double in ST0, not `int64_t`. Wrappers in `src/NeighborsSim641.h`.

## Water connection active flag

Water sim `0x6dd380`, state case 5 (near `0x6de6d6`): for each `GetConnectionsOfType(5)` connection, `0x6da480` clamps the connection cell into the city and calls `SetIsActive(waterGrid(+0x30)->vt[0x1c](x, z) & 1)`: active when the edge cell's grid bit 0 is set (pipe carrying water). Unknown: whether a city in deficit still sets that bit at the edge.

## Diagnosis for the 2026-10-18 run

BumperVille's published record: water Produced 3556, Demanded 13306 (`Apply: SetProduced(2, 3556)`, `SetDemanded(2, 13306)` in TuggersVille's log). TuggersVille: 3592 vs 3914. sellerAvail is negative for both, so no water offer in either direction, whatever the graph shows. `RegionSyncOffer` logs the inputs to settle it.
