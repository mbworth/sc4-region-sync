# CLAUDE.md

Agent onboarding for sc4-region-sync. Read this top to bottom before doing anything.

## What this is

A SimCity 4 DLL plugin (x86 Windows, C++20) that syncs the per-tile regional summary records between several running game instances, so the player can play multiple cities in one region at once and each city sees the others' growth without reloading. The host game is closed source and version-locked to 1.1.641.

Status (2026-10-17): builds on Linux, runs in two instances, Phase 1 and 1b done (records sync both ways and move the demand simulator). Phase 2 connections and deals done 2026-10-18: live mirroring of single-network connections with edge stubs, deals with amount follow, removals, writer liveness. Save lock (Q4, mutex plus SaveCity slot wrapper) and dual-lane mirroring (Q12) observed in game 2026-10-18; ferry is logged, not mirrored. Autosave verified safe with two instances (Q4 step 8). Three instances verified (Q13) and the ferry log line observed, same day. Q1 closed. Coordinator done 2026-10-18 (Q14): status file, tile guard, `launcher/` with list, live thumbnail grid, Settings dialog and orphan-process End Task. First play soak clean (Q15); the demand saw-tooth it surfaced is fixed (Q16, FreezeExtrapolation). Q17 (store-change pull, region-view reload) built 2026-10-19, awaiting its in-game run. Q17 done 2026-10-19: store-change pulls on the sim-day tick and region-view record reload on peer save, both on by default; the tile picture does not redraw yet (texture rebuild path under research). Next: longer soaks, the picture redraw, then the Simtropolis item. `docs/PHASES.md` has the checkboxes.

## Read in this order

1. `docs/DESIGN.md`: why the regional record is the seam, why a file store, what deliberately stays local.
2. `docs/ARCHITECTURE.md`: how the game loads plugins, the object graph we touch, process topology, data flow, lifecycle, source map, build pipelines, constraints.
3. `docs/OPEN-QUESTIONS.md`: the experiments (Q1 to Q15). Each has an "Observed" slot. Empty slots are the work.
4. `docs/PHASES.md`: ordered plan with exit criteria and current position.
5. `src/RegionSyncDllDirector.cpp`: the entry point. Everything else is a helper.

Then skim `vendor/gzcom-dll/gzcom-dll/include/` for `cISC4RegionalCity.h`, `cISC4NeighborsSimulator.h`, `cISC4Region.h`, `cISC4App.h`, `cISC4Simulator.h`. Those five headers are the whole surface Phase 1 uses. Phase 2 adds `cISC4NeighborDeal.h` and `cISC4NeighborConnection.h`. `launcher/` is a separate Win32 exe (`make launcher`) that only reads the `_instances` status files; it never links the game headers.

## First session checklist (Linux box)

```
git submodule update --init --recursive      # if vendor/ dirs are empty
scripts/setup-toolchain.sh --prebuilt        # LLVM 20 into ~/.local/llvm-20 (no root), fetches MSVC/SDK via xwin (~1 GB, once)
make                                         # -> build/SC4RegionSync.dll + .ini
make test                                    # host-side snapshot round-trip test
```

`--apt` instead of `--prebuilt` only works where apt ships clang 19+; the MSVC STL that xwin fetches refuses older compilers (Debian 12 apt has clang 14). The Makefile finds `~/.local/llvm-20` on its own. The build compiled clean on 2026-10-17 with zero warnings from our sources.

Test loop: `make deploy` copies DLL, INI and the launcher exe to the Windows box over SSH (game and launcher must be closed), `Start-ScheduledTask SC4Launcher` starts the launcher on the desktop (or `SC4Instance1`..`3` for a bare instance), the human opens a city and types cheats, `make logs` pulls each instance's `SC4RegionSync.log` into `ingame_tests/` (over base64: plain scp truncates at 200 KB). The log is the only instrument we have; treat it as such. Ghidra 11 headless is set up for the exe (`tools/ghidra/`, project in `ingame_tests/ghidra`, gitignored).

## Ground rules

- **This repo is public.** Never commit personal, private, or sensitive information: hostnames, LAN addresses, usernames, real `C:\Users\<name>` paths, keys, screenshots, or raw in-game logs. Deploy targets live in gitignored `local.mk`. Log excerpts quoted in docs use `C:\Users\player`. Grep the diff for these before every commit.
- **Vendored headers are the contract.** Every `cISC4*` interface is reverse-engineered. Never reorder, insert, or remove virtual methods. If a header is wrong, the fix is a local wrapper or a documented upstream change, not an edit under `vendor/`.
- **`vendor/` is submodules.** Do not edit. `vendor/xwin/` is generated and gitignored.
- **`NeighborSnapshot.{h,cpp}` stays game-free and Windows-free.** It is the one piece that can be unit-tested on any machine. Keep `make test` (tests/snapshot_roundtrip.cpp) passing.
- **Only the director talks to the game's message system and cheat manager.** Game-object plumbing lives there; logic lives in the bridge, settings, and snapshot files.
- **Log first, infer second.** When a code path touches a game method we have not exercised before, log inputs and outputs at Info, not Debug. If an experiment's conclusion depends on it, it is Info.
- **Record observations.** Finishing an experiment means writing what happened into the matching "Observed" slot in `docs/OPEN-QUESTIONS.md`, including the negative result and the log excerpt that shows it. An experiment without a recorded observation is not done.
- **Fresh IDs.** Message IDs, cheat IDs and the director ID are fixed 32-bit constants at the top of the director. New ones must be random and must not collide with Null45's published plugins (check his repos) or ours.
- **Never let an exception reach game code.** Wrap new `DoMessage` branches in try/catch and log.
- **Never write to `.sc4` save files or the Regions folder from the plugin.** Saves go through `cISC4App::SaveCity` only, and only when `AutoSaveEveryMonths` or the user asks. See Q4 before enabling autosave with two instances.

## Workflow per change

The global flow (plan, review, implement, review, test, deploy, verify) applies. Here "test" is `make test` plus the in-game experiment, and "verify" means the observation is written into `docs/OPEN-QUESTIONS.md`. Small changes still get the build-and-log step; there is no test suite that can stand in for the game.

Commit messages: imperative subject, body says what was observed in-game if anything.

## Things that look wrong but are not

- `cISC4Region::GetCity(x, z)` returns a pointer to a ref-counted **slot**, not a city pointer. `FindRegionalCity` in the bridge does the cast. Do not `AddRef` or `Release`.
- `cISC4Region::GetDirectoryName()` returns `SC4String*`, which is `cIGZString`-compatible and has `ToChar()`.
- `cISC4RegionalCity` has setters for budget, income and commodity stats but none for population or jobs; `src/RegionalCity641.h` writes those fields directly at offsets confirmed in Ghidra. The vendored header also declares float where the binary uses double; the wrapper is the fix, not the header.
- `version.rc` is UTF-16 LE with a BOM. MSVC's rc and `llvm-rc` both want it that way. Do not convert it.
- The Makefile passes `/LD` and `/MD`: dynamic CRT, same as the vcxproj. The game ships without a static CRT dependency on us; the player needs the VC++ 2022 x86 redistributable, which every other DLL plugin also requires.

## When stuck on a game interface

1. Check whether Null45 has already called it: `grep -rn MethodName` across his repos (`sc4-auto-run-cheats`, `sc4-region-census`, `sc4-growify`, `sc4-more-building-styles` and others on github.com/0xC0000054).
2. Check `nam-dll` (github.com/NAMTeam/nam-dll) for the same.
3. If nobody has, treat the call as an experiment: log before, call, log after, in a cheat command, not on a timer, so it only runs when a human types it.
4. Only then reach for Ghidra (`tools/ghidra/*.py`, known vtables and offsets are listed in `docs/OPEN-QUESTIONS.md`).

## What not to do

- Do not try to run or simulate two cities inside one process. Not the design; see `docs/DESIGN.md`.
- Do not replace the file store with IPC until Phase 1 has shown the file store is the bottleneck. It almost certainly is not.
- Do not add a UI. Cheats plus the log are enough until the mechanism is proven.
- Do not "fix" the upstream `SC4String` size assert or the missing `<utility>` include in `SC4Vector.h` that show up when syntax-checking on 64-bit Linux without xwin. They are artifacts of the host, not bugs.
