# Plan: save lock (Q4)

Research: `docs/research/save-641.md`. Reviewed 2026-10-18 in two rounds (design, safety, then the vtable wrapper). Round one replaced the lock file plus posted release message with a kernel mutex plus a vtable wrapper; round two fixed the wrapper ABI, the hold ownership and the mutex lifetime.

## Problem

Every save (`0x455e20`, reached only through App vtable byte offset 0x3c: Ctrl+S, the menu, `RequestSaveCity`, our `Save()`) does, in order: write a temp file, close the read handle on the own `.sc4`, rename `own.sc4 -> own.sc4_`, rename `temp -> own.sc4`, reopen the own file read-only, then open each adjacent neighbor's `.sc4` read-write (`OPEN_ALWAYS`, no `FILE_SHARE_DELETE`) and write this process's copy of that neighbor's record.

With two instances A and B adjacent:

- While A has its city open, B's neighbor write into `A.sc4` fails with a sharing violation and nothing reports it. Harmless.
- If B's neighbor write opens `A.sc4` between A's two renames, `OPEN_ALWAYS` creates an empty `A.sc4`; A's second rename fails, the new save is deleted, the real city is left as `A.sc4_`. Data loss.
- If B opens `A.sc4` just before A's first rename, A's save fails and rolls back. No loss, a failed save.
- If A's `ReloadCity` (RegionSyncReload) reads `B.sc4` during B's renames, B's save fails.

The windows are milliseconds wide; two autosaves or two Ctrl+S can still hit them. One region-wide lock over the whole save routine closes all of them.

Why not suppress the neighbor write instead (DESIGN.md's question): reloaded cities depend on the neighbor's saved copy of shared deals (Q9 step 4), and the reader collisions would remain.

## Mechanism: `src/SaveLock.{h,cpp}` (Windows-only, no game headers)

- Named mutex `Local\SC4RegionSync.SaveLock.<region directory name>` (`CreateMutexW`, null security attributes, name converted with `MultiByteToWideChar(CP_ACP)`; folder names hold no backslash, asserted). Both instances run in one logon session on one host, which is all the project supports (writer liveness is same-host too). Abandoned on crash by the kernel; no file, no store dependency, no stale state. The name must not contain `SimCity 4 Mutex` (the Unlock cheat matches by substring).
- **Lifetime.** `Open(regionDirectoryName)` in `PostRegionInit` (and the late `PostCityInitComplete` path that reaches it); `Close()` in `PreRegionShutdown` after a release. `GetLastError()==ERROR_ALREADY_EXISTS` is logged as `peer=1`: that, not the name string, proves both instances share the object. With no handle every caller runs unlocked.
- `Acquire(timeoutMs) -> {held, waitedMs, abandoned, depth}`: depth counter. Depth 0: `WaitForSingleObject`; `WAIT_ABANDONED` counts as held and is logged at Warning. Depth > 0: already held by this thread, returns at once and increments. Timeout: not held, depth unchanged, logged at Warning, caller proceeds. `Release()`: decrements; `ReleaseMutex` only at 0; no-op when not held. So a save inside a `RegionSyncHoldLock` hold, or a reload inside a save, never frees a hold it did not take.
- After a timeout, later `Acquire` calls wait 0 ms until one succeeds (a hung peer costs one timeout, not one per save). Every save that goes ahead unlocked logs at Warning.
- Every `SaveLock:` line carries local wall-clock `HH:mm:ss.fff` (`GetSystemTimePreciseAsFileTime` + `FileTimeToLocalFileTime`) so the two logs and `LastWriteTime` of the `.sc4` files line up; the `acquired` line is flushed.

## Hook: `src/SaveHook641.{h,cpp}` (game-facing, version-locked like `RegionalCity641.h`)

- Slot: `kSaveCityPathSlot = 15` (byte offset 0x3c) of the App vtable `0xa86af0`; the slot address is `0xa86b2c`, the routine `0x455e20`. The header's `SaveCity(bool)`/`SaveCity(path, bool)` order compiles to the right slots because MSVC and clang-cl reverse overloads.
- `Install(pApp, onEnter, onExit)`: require `*(void***)pApp == 0xa86af0` and `&vtbl[15] == 0xa86b2c`, else log and do not patch. If `vtbl[15]` is not `0x455e20` another plugin patched the slot first: chain to that value and log it with its owning module (`GetModuleHandleExW(FROM_ADDRESS)`) at Warning. `VirtualProtect` the page (`.rdata`), `InterlockedCompareExchangePointer` the slot, restore the old protection; log the old protection, slot value before and after and both `VirtualProtect` results at Info. The project has never written to `.rdata` before.
- Wrapper: `static bool __fastcall SaveCityWrapper(cISC4App* self, void* edx, const cIGZString& path, bool fast)` (ecx = this, edx ignored, two stack args, callee pops 8: matches `RET 8`; a `__thiscall` free function does not build under MSVC and the vcxproj must keep building). Original kept as `bool(__thiscall*)(cISC4App*, const cIGZString&, bool)`. Body: RAII guard that `Acquire`s on construction and `Release`s on destruction, call the original, return its result; our side is wrapped in try/catch, the original is always called. A re-entry counter logs depth > 1 at Warning (a nested pump inside the save would show here). After `make`, `llvm-objdump -d` must show the wrapper ending in `ret 8`.
- `Uninstall()`: restore `0x455e20` (or the chained value) only if the slot still holds our wrapper. Called from a new `PreAppShutdown` override (logged) and from the director destructor (no logging, no mutex calls: the Logger may be gone at DLL detach; the kernel abandons the mutex).
- Installed in `PostAppInit` when `SaveLock=1`; the Initialized line logs `saveLock=%d hook=%d`.

No message subscriptions, no posted release, no special case for plugin saves (`RequestSaveCity` goes through the same slot).

Other lock users, director only:

- `ReloadAdjacent`: guard around the `ReloadCity` loop. `SaveLock=0` leaves it unlocked, as today.
- `PostCityShutdown`: release the cheat hold if on (Info).

Assumption, tested by experiment step 9: nothing inside `0x455e20` pumps messages or shows a modal dialog on the success path. A failure dialog inside the save (disk full) would hold the lock while it is open; the peer then pays one timeout and continues unlocked, logged.

## Settings

`[Sync] SaveLock=1` (code default true, the Q4 observation decides whether it stays), `SaveLockTimeoutMs=5000`. 0 means try once, never wait. Step 1's `held=` across a large city, fast and full save, is the evidence for the default.

## Cheat

`RegionSyncHoldLock` (new random id, checked against existing ids and Null45's repos), a toggle: first use takes a hold (`Acquire`, depth 1), second use or city shutdown releases it. No `Sleep` on the main thread, so the holding instance can still be saved, closed or killed during the hold. Diagnostic only.

## What must not break

- A save is never blocked for longer than the timeout and never cancelled or failed by the plugin.
- Never write to `.sc4` or Regions from the plugin: nothing in this change touches a file.
- One instance only: `Acquire` returns at once; cost is one wait per save.
- The patch is applied only when the App vtable is the 641 one; any other exe leaves saves untouched.
- Cooperative: `SaveLock=0` in one instance disables protection for both; `peer=` and `hook=` in both logs make a mismatch visible.
- Autosave stays off by default; the lock does not prevent the stale-copy case below.

## Not covered (record in Q4)

- B saving while A sits in the region view or is closed writes B's copy of A's record into `A.sc4` (not a race; the lock cannot help). B's copy is at most one pull old; whether that ever regresses A's saved state is experiment step 8 and decides whether autosave can be turned on.
- City load and region view load read neighbor `.sc4` files; a collision with a peer's renames fails the peer's save cleanly, no loss. Same mechanism on `0x26C63340`/`0x26C63342` if a log ever shows it.
- Region view operations (`0x4a7200`, MoveCity) that write neighbor files.
- Other hosts, or two instances in different logon sessions: not supported, as for liveness.

## Experiment (Q4)

Both instances, `SaveLock=1`. File times: `gci <region> -Filter *.sc4* | % { '{0} {1:HH:mm:ss.fff}' -f $_.Name, $_.LastWriteTime }` (never CreationTime: NTFS tunnels it through the rename).

0. Install: both logs show the vtable and slot checks, slot value before and after, the protection change, `peer=1`, `hook=1`. Restart one instance with `SaveLock=0`: `hook=0`, saves still work.
1. Ctrl+S in A alone: `SaveLock: acquired waited=0 ms depth=1` then `SaveLock: released held=N ms`, both stamped; the own `.sc4` and any rewritten neighbor `.sc4` times fall between them. Same for `RegionSyncSave`. Note `held=` for a fast and a full save.
2. `SaveLockTimeoutMs=30000` in A. `RegionSyncHoldLock` in B, Ctrl+S in A (A freezes), then toggle B off: A logs `waited≈` the time until the toggle, save completes, files intact.
3. Hold in B, B presses Ctrl+S itself (`depth=2` on its save, hold kept), then A saves: A must still wait until B toggles off.
4. `SaveLockTimeoutMs=2000`, B holding: A logs `warning, timeout after 2000 ms, going ahead unlocked`, A's next save logs `warning, unlocked (peer still holding since last timeout)` without blocking while B still holds, and after B releases a later save acquires normally.
5. Save A while B's city is open: `B.sc4` time unchanged (sharing violation predicted). Save A while B sits in the region view: `B.sc4` rewritten. Record which; it settles the earlier timestamp note.
6. `RegionSyncReload` in A: acquire and release around the reload lines.
7. B holds, then `taskkill /F` on B's pid: A's next save logs `acquired (abandoned)` and completes. B holds, then Exit button (no `PostCityShutdown`): A's next save logs `acquired (abandoned)` unless B's app shut down cleanly (`PreAppShutdown` closes the mutex, then no `abandoned`). Save and exit to region, and quit with save, in A: `released` logged, B's next save `waited=0`.
8. Autosave both at `AutoSaveEveryMonths=1`, same sim speed, a few months: every tick acquires and releases (collisions are rare, so also let B hold across one of A's months and expect a wait). Then the stale-copy case: save A, exit A to region, let B autosave, reopen A, `RegionSyncDump`: compare A's population, budget and deals with its last save. A regression keeps autosave off or adds a final publish before city shutdown.
9. Sim paused, Ctrl+S: same as 1, `depth` never above 1.

## Docs and build to amend

`DESIGN.md` "Save contention" (mutex plus wrapper, not a lock file), `ARCHITECTURE.md` (lifecycle: the slot patch and `PreAppShutdown`; source map; constraints: main-thread wait bound), `PHASES.md` (split "save lock" and "turn on autosave"), `README.md` (cheat row, settings), `CLAUDE.md` status line, `Makefile` and `src/SC4RegionSync.vcxproj` (`SaveLock.cpp`, `SaveHook641.cpp`), Q4 in `OPEN-QUESTIONS.md`.
