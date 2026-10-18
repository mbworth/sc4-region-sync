# Plan: save lock (Q4)

Research: `docs/research/save-641.md`. Reviewed 2026-10-18 (design and safety lenses); the first draft's lock file plus posted release message was replaced by a kernel mutex plus a vtable wrapper on both reviewers' findings.

## Problem

Every save (`0x455e20`, reached only through App vtable slot 0x3c: Ctrl+S, the menu, `RequestSaveCity`, our `Save()`) does, in order: write a temp file, close the read handle on the own `.sc4`, rename `own.sc4 -> own.sc4_`, rename `temp -> own.sc4`, reopen the own file read-only, then open each adjacent neighbor's `.sc4` read-write (`OPEN_ALWAYS`, no `FILE_SHARE_DELETE`) and write this process's copy of that neighbor's record.

With two instances A and B adjacent:

- While A has its city open, B's neighbor write into `A.sc4` fails with a sharing violation and nothing reports it. Harmless.
- If B's neighbor write opens `A.sc4` between A's two renames, `OPEN_ALWAYS` creates an empty `A.sc4`; A's second rename fails, the new save is deleted, the real city is left as `A.sc4_`. Data loss.
- If B opens `A.sc4` just before A's first rename, A's save fails and rolls back. No loss, a failed save.
- If A's `ReloadCity` (RegionSyncReload) reads `B.sc4` during B's renames, B's save fails.

The windows are milliseconds wide; two autosaves or two Ctrl+S can still hit them. One region-wide lock over the whole save routine closes all of them.

Why not suppress the neighbor write instead (DESIGN.md's question): reloaded cities depend on the neighbor's saved copy of shared deals (Q9 step 4), and the reader collisions would remain.

## Mechanism

`src/SaveLock.{h,cpp}`, Windows-only, no game headers:

- Named mutex `Local\SC4RegionSync.SaveLock.<region directory name>` (`CreateMutexW`, null security attributes). Both instances are on one host, which is all the project supports (writer liveness is same-host too). Abandoned on crash by the kernel; no file, no store dependency, no stale state. The name must not contain `SimCity 4 Mutex` (the Unlock cheat matches by substring).
- `Acquire(timeoutMs) -> {held, waitedMs, abandoned}`: `WaitForSingleObject`; `WAIT_ABANDONED` counts as held and is logged at Warning. Idempotent: a call while held returns held with `waited=0` (a Win32 mutex is recursive; the bool guard keeps one `ReleaseMutex` per hold). Timeout: not held, logged, caller proceeds.
- `Release()`: no-op when not held.
- After a timeout, later `Acquire` calls wait 0 ms until one succeeds, so a hung peer costs one timeout, not one per save.
- Every log line carries a wall-clock `HH:mm:ss.fff` (`GetSystemTimePreciseAsFileTime`) so the two instances' logs and file times can be lined up.

## Hook: App vtable slot 0x3c wrapper

`src/SaveHook641.{h,cpp}` (game-facing, version-locked like `RegionalCity641.h`):

- `Install(pApp, onEnter, onExit)`: read the App vtable from the `cISC4App*`, require slot 0x3c to hold `0x455e20` (else log and do not patch), `VirtualProtect` the one pointer, write the wrapper, restore protection, keep the original.
- Wrapper `bool __thiscall(cISC4App*, const cIGZString& path, bool fast)`: `Acquire`, call the original, `Release`, return its result. Brackets steps 1 to 10 of the routine on every save path by construction, success and failure alike. Exceptions from our side are caught around `Acquire`/`Release`; the original is always called.
- `Uninstall()` in `PreAppShutdown` and the director destructor: restore the original pointer if it still holds our wrapper.
- Installed in `PostAppInit` when `SaveLock=1`; the Initialized line logs `saveLock=%d hook=%d`.

No message subscriptions, no posted release, no special case for plugin saves (`RequestSaveCity` goes through the same slot).

Other lock users, director only:

- `ReloadAdjacent`: `Acquire` before the `ReloadCity` loop, `Release` after. `SaveLock=0` leaves it unlocked, as today.
- `PostCityShutdown` and the destructor: `Release` if held (Info; a hold there means the hold cheat was left on).

## Settings

`[Sync] SaveLock=1` (code default true, the Q4 observation decides whether it stays), `SaveLockTimeoutMs=5000` (Windows ghosts an unpumped window after about 5 s; a real save takes well under that). 0 means try once, never wait.

## Cheat

`RegionSyncHoldLock` (new random id), a toggle: first use `Acquire` and keep the hold, second use or city shutdown `Release`. Lets the player save in the other instance while this one holds the lock, with no `Sleep` on the main thread (a sleeping instance could not be killed or closed during the hold, which step 6 needs).

## What must not break

- A save is never blocked for longer than the timeout and never cancelled or failed by the plugin.
- Never write to `.sc4` or Regions from the plugin: nothing in this change touches a file.
- One instance only: `Acquire` returns at once; cost is one wait per save.
- The patch is applied only when slot 0x3c holds the expected address; any other exe leaves saves untouched.
- Cooperative: `SaveLock=0` in one instance disables protection for both. Both logs show the mutex name at startup so a mismatch is visible.
- Autosave stays off by default; the lock does not prevent the stale-copy case below.

## Not covered (record in Q4)

- B saving while A sits in the region view or is closed writes B's copy of A's record into `A.sc4` (not a race; the lock cannot help). B's copy is at most one pull old; whether that ever regresses A's saved state is experiment step 8 and decides whether autosave can be turned on.
- City load and region view load read neighbor `.sc4` files; a collision with a peer's renames fails the peer's save cleanly, no loss. Same mechanism on `0x26C63340`/`0x26C63342` if a log ever shows it.
- Region view operations (`0x4a7200`, MoveCity) that write neighbor files.
- Other hosts over a network share: not supported, as for liveness.

## Experiment (Q4)

Both instances, `SaveLock=1`.

1. Ctrl+S in A alone: `SaveLock: acquired waited=0 ms` then `SaveLock: released held=N ms`, both with timestamps; the own `.sc4` and any rewritten neighbor `.sc4` times (`region.ps1`, `.fff`) fall between them. Same for `RegionSyncSave`.
2. `RegionSyncHoldLock` in B, Ctrl+S in A, then toggle B off: A logs `acquired waited≈` the hold's remaining time, save completes, files intact.
3. `SaveLockTimeoutMs=2000`, B holding: A logs `timeout after 2000 ms, saving anyway`, then the next A save logs `waited=0` without blocking while B still holds, then after B releases a later save acquires normally.
4. Save A while B's city is open: `B.sc4` time unchanged (sharing violation predicted). Save A while B sits in the region view: `B.sc4` rewritten. Record which; it settles the earlier timestamp note.
5. `RegionSyncReload` in A: acquire and release around the reload lines.
6. B holds the lock, then `taskkill /F` on B's pid: A's next save logs `acquired (abandoned)` and completes. Repeat with the Exit button.
7. Save and exit to region in A, and quit with save: `released` in the log, and B's next save `waited=0`.
8. Autosave both at `AutoSaveEveryMonths=1`, same sim speed, a few months: every tick acquires and releases, no timeout. Then the stale-copy case: save A, exit A to region, let B autosave, reopen A, `RegionSyncDump`: compare A's population, budget and deals with its last save. A regression keeps autosave off or adds a final publish before city shutdown.
9. Sim paused, Ctrl+S: same as 1 (no modal loop pumps anything inside the wrapper, by construction; this confirms no hang).

## Docs and build to amend

`DESIGN.md` "Save contention" (mutex plus wrapper, not a lock file), `ARCHITECTURE.md` (lifecycle: the slot patch; source map; constraints: main-thread wait bound), `PHASES.md` (split "save lock" and "turn on autosave"), `README.md` (cheat row, settings), `CLAUDE.md` status line, `Makefile` and `src/SC4RegionSync.vcxproj` (`SaveLock.cpp`, `SaveHook641.cpp`), Q4 in `OPEN-QUESTIONS.md`.
