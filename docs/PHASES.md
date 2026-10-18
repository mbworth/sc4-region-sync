# Phases

## Phase 0: build and load (this scaffold)

- [x] Build `SC4RegionSync.dll` (x86, C++20). Done 2026-10-17 on Linux via clang-cl 20 + xwin; see CLAUDE.md.
- [x] Copy to Plugins with `SC4RegionSync.ini`. Load a city. Confirm `SC4RegionSync.log` appears next to the DLL and shows "City loaded". Done 2026-10-17 (`ingame_tests/`).
- [x] `RegionSyncStatus` shows a dialog. `RegionSyncDump` writes the region table, connections and deals to the log. Done 2026-10-17.
- [x] Answer Q6 (commodity indices). 1 = power, 2 = water, 3 = garbage (2026-10-17).

Exit criterion: the plugin loads, the cheats work, and the log tells us what the game thinks the region looks like.

## Phase 1: file-backed sync, single install, two instances

- [x] Answer Q5 (two instances). Done 2026-10-17: mutex closed by the plugin plus steam_appid.txt for the Steam build.
- [x] Answer Q1 (UpdateCityCache side effects). In-memory only; no region file changes over a nine-minute session with no saves (2026-10-18).
- [x] With `Enabled=1`, run two adjacent tiles. Confirm each instance's log shows monthly publishes and pulls, and that pull reports `changedFields > 0` after the other instance grows. Done 2026-10-17.
- [x] Answer Q2 (does demand move). No: records update live, demand does not (2026-10-17). Phase 1b.

Exit criterion: growth in instance B visibly changes regional demand in instance A without A reloading its city. Met 2026-10-17 via Phase 1b (fake neighbor test; a real two-instance repeat is still worth one run).

## Phase 1b: demand refresh (done)

- [x] The simulator caches regional population at load (Q2). Instead of `ReloadCity`, the director mirrors the simulator's own init: after every pull it feeds the change in regional extrapolated population into the 12 region-use demand objects (`AddToSupplyValue`). Verified in game 2026-10-17.
- [x] Two-instance confirmation: TuggersVille grew 17.5k -> 21.8k, BumperVille's record and demand supply followed month by month with no drift (2026-10-17, after fixing the publish feedback loop and stale snapshots).
- [x] Target fix (Q2 correction, 2026-10-17): the feed goes into `GetDemand(group, 0x10000)`, not index 0, and the publish is raw. The load check (`RegionalDemand: <group> use=1 ... target supply=`) has shown in every run since 2026-10-18.
- [ ] Q3 (`ReloadCity`) is now optional; keep for the thumbnail refresh in Phase 3.

## Phase 2: hardening

- [x] Population and RCI-group setters on the regional record. Direct field writes at +0x24/0x28/0x2c and the type map at +0x3c (Ghidra, 2026-10-17).
- [x] Save lock (Q4): kernel mutex across the SaveCity slot, observed 2026-10-18 (wait, timeout, abandoned, neighbor writes inside the lock).
- [x] Autosave with two instances: Q4 step 8 soak and stale-copy check passed 2026-10-18. Shipped default stays off; `AutoSaveEveryMonths=1` is safe to set.
- [x] Connections and deals between running instances: see `docs/PLAN-PHASE2.md`. All slices done (the items below). Slice 1 (mirror single-cell connections and deals, create only) done 2026-10-18: Q9 steps 1 to 4 observed, on by default, save and reload produce no duplicates.
- [x] Amount follow (slice 2a): done 2026-10-18, observed both ways.
- [x] Removal of connections and deals (slice 2c) and edge network stubs (slice 2b): observed with the cheat 2026-10-18 (Q10, Q11), on by default, no-cheat run observed the same day.
- [x] Three instances at once: Q13 observed 2026-10-18 (pulls, mirror, save lock queueing, dead writer, restart). Ferry log line observed (Q12 step 6).
- [x] Dual-lane networks (avenue, highway, ground highway) mirrored as pairs with a 2 by 3 stub: observed 2026-10-18 for all three types (`docs/PLAN-DUAL-FERRY.md`, Q12).
- [x] Ferry: not mirrored by design (each city's terrain scan makes its own; deals never use it); logged from the cheat.
- [ ] Replace the monthly file poll with a change notification (`ReadDirectoryChangesW` on the store) if the cadence feels laggy.
- [ ] Separate installs: make `StoreDirectory` a required setting when the DLL detects it is not under a shared Plugins folder.

## Phase 3: nice to have

- [ ] Region view in each instance refreshes thumbnails for tiles the other instance is playing.
- [ ] Coordinator slice 1 (`docs/PLAN-LAUNCHER.md`): status file, same-tile guard (tile mutex, second opener stops writing), Win32 launcher with list, raise, launch gate and resource estimate. Implemented 2026-10-18, Q14 pending.
- [ ] Coordinator slice 2: grid of live DWM thumbnails, Close button after the WM_CLOSE probe (Q14 step 5).
- [ ] Publish to Simtropolis once it has survived a few hundred hours of play.
