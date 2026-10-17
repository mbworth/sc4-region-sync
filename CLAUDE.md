# CLAUDE.md

Context for agents working in this repo.

## What this is

A SimCity 4 DLL plugin (x86, C++20, MSVC v143) that syncs the per-tile regional summary records between several running game instances so the player can play multiple cities in one region concurrently. Read `docs/DESIGN.md` first, then `docs/OPEN-QUESTIONS.md`. The design is a hypothesis until Q2 in that file is answered in-game.

## Ground rules

- The game is closed source. Every `cISC4*` interface in `vendor/gzcom-dll` is reverse-engineered. Treat vtable order as load-bearing: never reorder, insert or remove virtual methods in vendored headers. If a method is wrong, fix it upstream or add a local header that documents the deviation.
- Do not edit anything under `vendor/`. Those are submodules.
- `NeighborSnapshot.{h,cpp}` must stay free of game and Windows includes so it can be compiled and tested anywhere. There is a round-trip test idea in `docs/PHASES.md`; keep it passing.
- Only the director (`RegionSyncDllDirector.cpp`) subscribes to messages and registers cheats. Keep game-object plumbing there and logic in the bridge/settings/snapshot files.
- Logging is the product in the research phases. When adding a code path that touches a game object we have not exercised before, log inputs and outputs at Info level, not Debug.
- Message IDs and cheat IDs are fixed 32-bit constants. New IDs must be fresh random values; check they do not collide with the ones at the top of the director or with Null45's plugins.

## Building

No MSVC here means no real build in a Linux sandbox. For a syntax pass on the game-independent files:

```
clang++ -std=c++20 -fsyntax-only -I src -I vendor/gzcom-dll/gzcom-dll/include -I vendor/sc4-dll-utilities/sc4-dll-utilities/include src/NeighborSnapshot.cpp src/RegionalCityBridge.cpp src/SyncSettings.cpp
```

The director needs `Windows.h` and the full SDK; two known upstream header errors on 64-bit Linux (`SC4String` size assert, missing `<utility>` for `std::exchange` in `SC4Vector.h`) are not ours. The real build is `msbuild src\SC4RegionSync.sln /p:Platform=Win32` on Windows.

## Workflow per feature

Plan, adversarial review of the plan, implement, adversarial review of the code, deploy to the game, run the matching experiment from `docs/OPEN-QUESTIONS.md`, record the observation in that file. An experiment with no recorded observation is not done.

## Things that look wrong but are not

- `cISC4Region::GetCity` returns a pointer to a ref-counted slot, not a city pointer. `FindRegionalCity` in the bridge does the cast. Do not AddRef or Release it.
- `GetDirectoryName` on the region returns `SC4String*`, which is `cIGZString` compatible and has `ToChar`.
- `cISC4RegionalCity` has setters for budget, income and commodity stats but none for population or jobs. That gap is Q2, not an oversight in the bridge.
