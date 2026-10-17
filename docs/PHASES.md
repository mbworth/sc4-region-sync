# Phases

## Phase 0: build and load (this scaffold)

- [x] Build `SC4RegionSync.dll` (x86, C++20). Done 2026-10-17 on Linux via clang-cl 20 + xwin; see CLAUDE.md.
- [x] Copy to Plugins with `SC4RegionSync.ini`. Load a city. Confirm `SC4RegionSync.log` appears next to the DLL and shows "City loaded". Done 2026-10-17 (`ingame_tests/`).
- [x] `RegionSyncStatus` shows a dialog. `RegionSyncDump` writes the region table, connections and deals to the log. Done 2026-10-17.
- [x] Answer Q6 (commodity indices). 1 = power, 2 = water, 3 = garbage (2026-10-17).

Exit criterion: the plugin loads, the cheats work, and the log tells us what the game thinks the region looks like.

## Phase 1: file-backed sync, single install, two instances

- [x] Answer Q5 (two instances). Done 2026-10-17: mutex closed by the plugin plus steam_appid.txt for the Steam build.
- [ ] Answer Q1 (UpdateCityCache side effects).
- [ ] With `Enabled=1`, run two adjacent tiles. Confirm each instance's log shows monthly publishes and pulls, and that pull reports `changedFields > 0` after the other instance grows.
- [ ] Answer Q2 (does demand move). This is the go/no-go for the whole approach.

Exit criterion: growth in instance B visibly changes regional demand in instance A without A reloading its city.

## Phase 1b: reload path (only if Q2 says the simulator caches at load)

- [ ] Answer Q3 (`ReloadCity` mid-game).
- [ ] If it works: `AutoSaveEveryMonths=N` in B, a `ReloadEveryMonths` setting in A, and a save lock in the store so saves never overlap.
- [ ] If it does not: find the simulator's cached regional values (start with `cISC4DemandSimulator`) and refresh them directly after a pull.

## Phase 2: hardening

- [x] Population and RCI-group setters on the regional record. Direct field writes at +0x24/0x28/0x2c and the type map at +0x3c (Ghidra, 2026-10-17).
- [ ] Save lock (Q4). Turn on autosave.
- [ ] Deal propagation: when B proposes or cancels a deal, A should see it without a reload. Deals live on the regional record (`SetLocalDeals`) so this is the same mechanism, just a richer snapshot.
- [ ] Replace the monthly file poll with a change notification (`ReadDirectoryChangesW` on the store) if the cadence feels laggy.
- [ ] Separate installs: make `StoreDirectory` a required setting when the DLL detects it is not under a shared Plugins folder.

## Phase 3: nice to have

- [ ] Region view in each instance refreshes thumbnails for tiles the other instance is playing.
- [ ] A tray coordinator that launches instances, shows which tile each one has open, and prevents two instances from opening the same tile.
- [ ] Publish to Simtropolis once it has survived a few hundred hours of play.
