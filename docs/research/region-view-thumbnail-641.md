# Region view tile pictures (SC4 1.1.641), static analysis

Ghidra only, nothing run in game. Goal: why a tile's picture stays stale after `cISC4Region::ReloadCity` in the region view (Q17 step 4), and what the plugin can call to redraw it. Cross-checked against two public reverse-engineering notes on the same exe (Drexel-Macintosh/SimCity4_UI_Modernization `tools/research/REGION-SCREEN.md`, kycube/SiMCP-4 `re/reports/regions/REPORT.md`); every address below was re-read in our Ghidra project unless marked "external".

## 1. Where the pictures live

The thumbnails are not part of the `cISC4RegionalCity` record. `0x5dda40` (the only reader of the PNGs) has exactly two callers, neither in the region object:

| Function | What | Confidence |
|---|---|---|
| `0x5dda40` | `bool __cdecl LoadCityThumbnails(const cIGZString* path, IGZBuffer** sat, IGZBuffer** satMask, IGZBuffer** transport, IGZBuffer** transportMask)`. Opens the `.sc4` read-only (`0x5dbfe0`, version 0x3E..0x41), calls `0x5dd480` (instances 0, 2, 4, 6), closes. Out params are refcounted. | High |
| `0x7b13c0` | Region screen `BuildCityItems`, called once from screen Init `0x7b1900` (`0x7b1fa6`). Only region-view caller of `0x5dda40`. | High |
| `0x7770b0` | City popup, handler for message `0x26D31EC0`; reloads its own two images (`+0x10`, `+0x14`) each time it fires. Not the tile picture. | Medium |

Object graph (screen and view vtables from our dump; field meanings match the external map):

| Object | Address / field | Notes |
|---|---|---|
| Region screen `cSC4WinRegionScreen` | vtable `0xab9260`, ctor `0x7b0c00` (new 0x200), window ID `0xEA659793` (ctor writes it to `+0x10`; Init calls SetID with it at `0x7b192e`) | QI returns `this` for cIGZWin, so the `cIGZWin*` is the object |
| Owner | `+0xEC` of the window built by `0x4e0d40` (create) / `0x4e0e70` (destroy); `0x4e1060` uses `kGZWin_WinSC4App` `0x6104489A` | Medium: the owner is the WinSC4App window |
| Screen `+0xE0` | `cSC4WinRegionView` (ctor `0x7b4090`, 0x128 bytes): item pointer list `+0x100/+0x104`, tile cache `+0x10C`, region id `+0xDC` | High |
| Screen `+0x118/+0x11C/+0x120` | `vector<item>`, stride 0x80, one per city tile | High |
| Screen `+0x1A4` | region index | High |

Item (0x80 bytes) fields we touch:

| Off | Meaning |
|---|---|
| `+0x08/+0x0C` | region grid x, z (same as `GetCity(x, z)`) |
| `+0x18` | size class 0/1/2 |
| `+0x1C/+0x20` | satellite thumbnail and its mask (from the `.sc4`, or default art) |
| `+0x24/+0x28` | transport thumbnail and mask |
| `+0x2C` | composite buffer, the actual blit source |
| `+0x34` | "composite built" byte; 0 forces a rebuild on next paint |
| `+0x44/+0x50/+0x5C` | run lists from the mask (click mask, hover frame) |

## 2. Why the picture is stale

`BuildCityItems` (`0x7b13c0`) runs only from screen Init. Per city it: assigns default art (`0x7abb80`), gets the city's save path (RC slot `0x7c`), releases `+0x1C..+0x28`, calls `0x5dda40` into them (`0x7b16b9`), falls back to `0x7abb80` on failure or size mismatch, then builds the composite (`0x7ae510`) and registers the item with the view (`0x7b5d50`). Painting (`0x7b4150` via the tile cache) only copies `+0x1C` into `+0x2C` when `+0x34` is clear; it never rereads the file.

`ReloadCity` (Region slot 15, `0x4a5030` external) swaps the record behind `GetCity(x, z)`. Nothing in it reaches the screen's item vector, so the pictures stay as loaded when the screen was created. Population, name and the popup come from the record (or the popup's own reload), which is why hover updates. Confidence: high (two callers of the only reader).

## 3. What the game does to refresh a tile in place

Delete City in the region view (`0x7b0099`, in the screen's button handler `0x7afaa0`) refreshes one item without rebuilding the screen. `ESI` = screen, `EDI` = item:

| # | Call | Address |
|---|---|---|
| 1 | `region->DeleteCity(&ref)` (slot `0x38`) | `0x7b00ec` |
| 2 | `0x7abb80(screen, item)` thiscall: default art into `+0x1C/+0x20`, release `+0x24/+0x28` | `0x7b00f2` |
| 3 | `0x7ae510(screen, item)` thiscall: rebuild composite `+0x2C`, mask run lists, label anchor | `0x7b00fa` |
| 4 | `0x7b5430(view = screen+0xE0, item)` thiscall: mark every cache cell dirty, clear every item's `+0x34`, rebuild the item's airport/seaport icons, request repaint | `0x7b0106` |
| 5 | `0x7ac110(screen)` panels, `0x7aec00(screen)` region population total | `0x7b010d`, `0x7b0114` |

Delete Region (`0x7b0059`) instead calls `cISC4App::RequestGoToRegionView(false)` (App slot 12, `0x4543b0`), which calls `CloseCity()` (slot 14) and then, with no city open, destroys and recreates the region screen (`0x4e0e70`, `0x4e0d40`), so `BuildCityItems` runs again for every tile.

## 4. What the plugin can call

Both run on the main thread, in the region view only (our idle service already gates on `regionViewReady`), after `ReloadCity`.

### A. Whole view: `cISC4App::RequestGoToRegionView(false)` (recommended first)

| | |
|---|---|
| Call | `pApp->RequestGoToRegionView(false)`, vendored header slot 12 (`0xa86b20` -> `0x4543b0`) |
| Effect | Screen torn down and rebuilt; every tile's thumbnail reread from its `.sc4` |
| Confidence | Medium-high. Same call the game makes from inside the region view after Delete Region. Not traced: `CloseCity` (`0x455330`) with no city open, and whether the rebuild sends a region-init message (it posts `0xABB5BB44` via `0x7abb00` at screen Init). |
| Risks | Visible flicker and pan reset (Init restores pan from prefs or centres it). Must not run while a city is open (the bool=false path skips the save prompt and closes the city). Re-entrancy: do it from idle, never from inside a screen message. Our `RegionWatch` gate may see a new region-init; make sure it does not loop into another reload. Do it once per batch of reloaded tiles. |

### B. One tile: mirror Delete City (no rebuild)

```
cIGZWin* app  = mainWindow->GetChildWindowFromID(0x6104489A);   // WinSC4App
cIGZWin* scr  = app ? app->GetChildWindowFromID(0xEA659793) : nullptr;
uint8_t* s    = reinterpret_cast<uint8_t*>(scr);
void*    view = *(void**)(s + 0xE0);
// item = entry in [*(u8**)(s+0x118), *(u8**)(s+0x11C)) stride 0x80 with +0x08 == x, +0x0C == z
thiscall 0x7abb80(s, item);                          // default art
release and null item+0x1C, +0x20, +0x24, +0x28;
if (!0x5dda40(&path, item+0x1C, item+0x20, item+0x24, item+0x28)  // cdecl, path from GetCitySaveFilePath
    || !*(void**)(item+0x1C) || !*(void**)(item+0x20))
    thiscall 0x7abb80(s, item);
thiscall 0x7ae510(s, item);
thiscall 0x7b5430(view, item);
thiscall 0x7aec00(s);                                // optional: region population total
```

| | |
|---|---|
| Signatures | `0x7abb80`, `0x7ae510`, `0x7b5430`: `void __thiscall f(void* this, void* item)`, `RET 4`. `0x5dda40`: `bool __cdecl`, 5 args, caller pops `0x14`. `0x7aec00`: `void __thiscall f(void* screen)`. |
| Confidence | Medium. Every step is the game's own sequence (Delete City and `BuildCityItems`), but called from outside the screen's message handler, and the screen lookup by window ID is not verified in game. |
| Risks | Raw addresses, 1.1.641 only. The game also checks that image and mask sizes match (`0x7b16c7..0x7b17e9`, plus `0x7aa8e0`); a mismatch it would have rejected crashes nowhere we know of but may draw wrong, so compare `GetRect` sizes (buffer slot `0x30`) and fall back to default art. The path must be a `cIGZString`; `0x5dbfe0` uses it only through its vtable (Low-medium: not checked slot by slot). Never hold `item` across frames: the vector can reallocate and the screen is destroyed on city load. |

### Second-best facts (if neither is wanted)

The stale picture is cached in item `+0x1C/+0x20` (source) and `+0x2C` (composite) of `screen+0x118[...]`; `ReloadCity` touches neither. Clearing `+0x34` alone only recopies the same stale source.

## 5. Next steps

1. In game, log the screen lookup: `GetChildWindowFromID(0x6104489A)` then `(0xEA659793)`, print the pointer, `+0x1A4` (region index), item count `(+0x11C - +0x118) / 0x80`, and each item's x, z. Expect the count to equal the established tiles.
2. Try A in a cheat (`RegionSyncReload`, region view only). Check: picture updates, no save prompt, whether a `PostRegionInit` line follows, pan after rebuild.
3. If A flickers too much, try B in a cheat on one tile and compare.
4. Not traced: `0x455330` (CloseCity) with no city, `0x4a5030` (ReloadCity) body, and the item-size check `0x7aa8e0`.
