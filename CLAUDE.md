# CLAUDE.md

Agent onboarding for sc4-region-sync. Read this top to bottom before doing anything.

## What this is

A SimCity 4 DLL plugin (x86 Windows, C++20) that syncs the per-tile regional summary records between several running game instances, so the player can play multiple cities in one region at once and each city sees the others' growth without reloading. The host game is closed source and version-locked to 1.1.641.

Status at hand-off: scaffold complete, never built, never loaded in the game. The design is a hypothesis until Q2 in `docs/OPEN-QUESTIONS.md` has an observation recorded.

## Read in this order

1. `docs/DESIGN.md`: why the regional record is the seam, why a file store, what deliberately stays local.
2. `docs/ARCHITECTURE.md`: how the game loads plugins, the object graph we touch, process topology, data flow, lifecycle, source map, build pipelines, constraints.
3. `docs/OPEN-QUESTIONS.md`: the seven experiments. Each has an "Observed" slot. Empty slots are the work.
4. `docs/PHASES.md`: ordered plan with exit criteria. We are at Phase 0.
5. `src/RegionSyncDllDirector.cpp`: the entry point. Everything else is a helper.

Then skim `vendor/gzcom-dll/gzcom-dll/include/` for `cISC4RegionalCity.h`, `cISC4NeighborsSimulator.h`, `cISC4Region.h`, `cISC4App.h`, `cISC4Simulator.h`. Those five headers are the whole surface Phase 1 uses.

## First session checklist (Linux box)

```
git submodule update --init --recursive      # if vendor/ dirs are empty
scripts/setup-toolchain.sh --apt             # installs clang/lld/llvm, fetches MSVC/SDK via xwin (~1 GB, once)
make                                         # -> build/SC4RegionSync.dll + .ini
```

Expect the first `make` to surface a handful of compile errors in the director: this code was syntax-checked on Linux with clang but never compiled against the real Windows SDK. Fix them in our files, not in `vendor/`. Likely spots: `cRZBaseString` construction from `std::string_view`, `const` mismatches on interface methods, `SC4String*` handling from `cISC4Region::GetDirectoryName`, and anything around `snprintf`/`std::filesystem::path::string()` under `/UNICODE`.

A clean build is the Phase 0 entry point. The build output then goes to the Windows machine's `<install>\Plugins` folder (DLL + INI), the game is started, a city is opened, and `SC4RegionSync.log` next to the DLL is read. That log is the only instrument we have; treat it as such.

## Ground rules

- **Vendored headers are the contract.** Every `cISC4*` interface is reverse-engineered. Never reorder, insert, or remove virtual methods. If a header is wrong, the fix is a local wrapper or a documented upstream change, not an edit under `vendor/`.
- **`vendor/` is submodules.** Do not edit. `vendor/xwin/` is generated and gitignored.
- **`NeighborSnapshot.{h,cpp}` stays game-free and Windows-free.** It is the one piece that can be unit-tested on any machine. Keep a round-trip test passing (`g++ -std=c++20 -I src test.cpp src/NeighborSnapshot.cpp`).
- **Only the director talks to the game's message system and cheat manager.** Game-object plumbing lives there; logic lives in the bridge, settings, and snapshot files.
- **Log first, infer second.** When a code path touches a game method we have not exercised before, log inputs and outputs at Info, not Debug. If an experiment's conclusion depends on it, it is Info.
- **Record observations.** Finishing an experiment means writing what happened into the matching "Observed" slot in `docs/OPEN-QUESTIONS.md`, including the negative result and the log excerpt that shows it. An experiment without a recorded observation is not done.
- **Fresh IDs.** Message IDs, cheat IDs and the director ID are fixed 32-bit constants at the top of the director. New ones must be random and must not collide with Null45's published plugins (check his repos) or ours.
- **Never let an exception reach game code.** Wrap new `DoMessage` branches in try/catch and log.
- **Never write to `.sc4` save files or the Regions folder from the plugin.** Saves go through `cISC4App::SaveCity` only, and only when `AutoSaveEveryMonths` or the user asks. See Q4 before enabling autosave with two instances.

## Workflow per change

Plan, adversarial review of the plan, implement, adversarial review of the code, build, deploy to the game, run the matching experiment, record the observation. Small changes still get the build-and-log step; there is no test suite that can stand in for the game.

Commit messages: imperative subject, body says what was observed in-game if anything. Attribution lines are fine.

## Things that look wrong but are not

- `cISC4Region::GetCity(x, z)` returns a pointer to a ref-counted **slot**, not a city pointer. `FindRegionalCity` in the bridge does the cast. Do not `AddRef` or `Release`.
- `cISC4Region::GetDirectoryName()` returns `SC4String*`, which is `cIGZString`-compatible and has `ToChar()`.
- `cISC4RegionalCity` has setters for budget, income and commodity stats but none for population or jobs. That gap is Q2, not an oversight in the bridge.
- `version.rc` is UTF-16 LE with a BOM. MSVC's rc and `llvm-rc` both want it that way. Do not convert it.
- The Makefile passes `/LD` and `/MD`: dynamic CRT, same as the vcxproj. The game ships without a static CRT dependency on us; the player needs the VC++ 2022 x86 redistributable, which every other DLL plugin also requires.

## When stuck on a game interface

1. Check whether Null45 has already called it: `grep -rn MethodName` across his repos (`sc4-auto-run-cheats`, `sc4-region-census`, `sc4-growify`, `sc4-more-building-styles` and others on github.com/0xC0000054).
2. Check `nam-dll` (github.com/NAMTeam/nam-dll) for the same.
3. If nobody has, treat the call as an experiment: log before, call, log after, in a cheat command, not on a timer, so it only runs when a human types it.
4. Only then reach for Ghidra on `SimCity 4.exe`. The regional city class's population fields (Q2) are the first thing worth that effort.

## What not to do

- Do not try to run or simulate two cities inside one process. Not the design; see `docs/DESIGN.md`.
- Do not replace the file store with IPC until Phase 1 has shown the file store is the bottleneck. It almost certainly is not.
- Do not add a UI. Cheats plus the log are enough until the mechanism is proven.
- Do not "fix" the upstream `SC4String` size assert or the missing `<utility>` include in `SC4Vector.h` that show up when syntax-checking on 64-bit Linux without xwin. They are artifacts of the host, not bugs.
