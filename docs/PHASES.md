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
- [x] With `Enabled=1`, run two adjacent tiles. Confirm each instance's log shows monthly publishes and pulls, and that pull reports `changedFields > 0` after the other instance grows. Done 2026-10-17.
- [x] Answer Q2 (does demand move). No: records update live, demand does not (2026-10-17). Phase 1b.

Exit criterion: growth in instance B visibly changes regional demand in instance A without A reloading its city. Met 2026-10-17 via Phase 1b (fake neighbor test; a real two-instance repeat is still worth one run).

## Phase 1b: demand refresh (done)

- [x] The simulator caches regional population at load (Q2). Instead of `ReloadCity`, the director mirrors the simulator's own init: after every pull it feeds the change in regional extrapolated population into the 12 region-use demand objects (`AddToSupplyValue`). Verified in game 2026-10-17.
- [x] Two-instance confirmation: TuggersVille grew 17.5k -> 21.8k, BumperVille's record and demand supply followed month by month with no drift (2026-10-17, after fixing the publish feedback loop and stale snapshots).
- [ ] Target fix (Q2 correction, 2026-10-17): the feed now goes into `GetDemand(group, 0x10000)`, not index 0, and the publish is raw. Unverified until the next run shows the load check.
- [ ] Q3 (`ReloadCity`) is now optional; keep for the thumbnail refresh in Phase 3.

## Phase 2: hardening

- [x] Population and RCI-group setters on the regional record. Direct field writes at +0x24/0x28/0x2c and the type map at +0x3c (Ghidra, 2026-10-17).
- [ ] Save lock (Q4). Turn on autosave.
- [ ] Connections and deals between running instances: see `docs/PLAN-PHASE2.md`. Slice 1 (mirror single-cell connections and deals, create only) is implemented behind `SyncConnections`/`SyncDeals` and the `RegionSyncMirror` cheat; exit is Q9 recorded.
- [ ] Later slices from the plan: removal and amount follow, dual-cell networks and ferry, each with its own plan.
- [ ] Replace the monthly file poll with a change notification (`ReadDirectoryChangesW` on the store) if the cadence feels laggy.
- [ ] Separate installs: make `StoreDirectory` a required setting when the DLL detects it is not under a shared Plugins folder.

## Phase 3: nice to have

- [ ] Region view in each instance refreshes thumbnails for tiles the other instance is playing.
- [ ] A tray coordinator that launches instances, shows which tile each one has open, and prevents two instances from opening the same tile.
- [ ] Publish to Simtropolis once it has survived a few hundred hours of play.
