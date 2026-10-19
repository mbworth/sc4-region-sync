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
- [x] Replace the monthly file poll with a change notification: `FindFirstChangeNotification` on the store polled on the sim-day tick, observed 2026-10-19 (Q17 step 2, `docs/PLAN-WATCH.md`). Monthly pull stays as fallback.
- [ ] Two PCs on one region: research done 2026-10-19 (`docs/research/lan-two-pcs.md`): host-authoritative replication through the launchers, no shared drive. Next: `docs/PLAN-LAN.md` slice 1, then Q18.

## Phase 3: nice to have

- [x] Region view tile pictures: parked 2026-10-19. The screen rebuilds them on every region-view entry; an automatic redraw would blink the screen and a per-tile refresh needs raw screen addresses (`docs/research/region-view-thumbnail-641.md`). `RegionSyncRedraw` rebuilds on demand.

- [x] Region view reloads a peer's tile record when the peer saves (`ReloadOnPeerSave`, Q17 step 4, observed 2026-10-19: hover population updated live). Steps 5 to 7 observed the same day; default 1 since then.
- [x] Coordinator slice 1 (`docs/PLAN-LAUNCHER.md`): status file, same-tile guard (tile mutex, second opener stops writing), Win32 launcher with list, raise, launch gate and resource estimate. Observed 2026-10-18 (Q14).
- [x] Coordinator slice 2: grid of live DWM thumbnails, observed 2026-10-18 (Q14 step 6). No Close button: the game ignores WM_CLOSE (Q14 step 5).
- [x] Coordinator slice 3: Settings dialog for startup parameters (resolution, windowed, intro, CPU, renderer, extra args, limits), 2026-10-18.
- [x] Coordinator slice 4: orphan game processes listed with End Task, observed 2026-10-18 (Q15).
- [x] Demand saw-tooth from the game's neighbor extrapolation fixed by pinning the record cap (Q16, 2026-10-18).
- [x] First play soak: three cities, about an hour, saves and sync clean (Q15, 2026-10-18).
- [ ] Publish to Simtropolis once it has survived a few hundred hours of play.
