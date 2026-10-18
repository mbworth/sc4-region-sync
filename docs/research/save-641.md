# City save (SC4 1.1.641), static analysis

Ghidra only, nothing run in game. Goal: where a two-instance save lock can go.

## 1. Entry points

| Path | Function | Notes |
|---|---|---|
| `cISC4App::SaveCity(path, fast)` | `0x455e20` | App+0xc interface, vtable `0xa86af0` slot 0x3c (`0xa86b2c`). `RET 8`. No direct calls: every caller goes through this slot. |
| `cISC4App::SaveCity(fast)` | `0x454da0` | Slot 0x40. Gets `city->GetCitySaveFilePath`, calls slot 0x3c. |
| `RequestSaveCity(notif, fast)` | `0x455ae0` | Slot 0x24. Returns false with a dialog if `city->IsSaveDisabled()` (city slot 0x1a4), else gets the path and calls slot 0x3c, then shows the notification. Synchronous. Our `RequestSaveCity(false, true)` (director, Save cheat / autosave) lands here. |

Slot order: the header declares `SaveCity(bool)` first, but the binary has the path overload at 0x3c. MSVC (and clang-cl) put overloads in reverse order, so the vendored header still compiles to the right slots. The player's menu or Ctrl+S save and plugin saves all end up in `0x455e20`.

## 2. Messages

All four are sent with `cIGZMessageServer2::MessageSend` (server `[0x00b43ccc]`, slot 0xc), so delivery is **synchronous**, inside `0x455e20` on the main thread. Each is a `cGZMessage2Standard` with Void1 = the temp DBPF segment, Void2 = the temp path (cRZString), Void3 = the target path, Data4 = fastSave.

| ID | Sent at | When | Who listens (exe) |
|---|---|---|---|
| `0x26C63343` PreSave | `0x45614b` | Temp DBPF is open; nothing has been written yet | Own regional city `0x4abd40` (UpdateCityCache), `0x495f10` (sets "saving" flag +0xc0), `0x43f1b0`, `0x45c1fe`, `0x6a9470` |
| `0x26C63346` | `0x4561b3` | Right after 43, before any record write | none found |
| `0x26C63344` | `0x456354` | After the city and own record are serialized, only on success | none found |
| `0x26C63345` PostSave | `0x4563bd` | Right after 44, only on success. **Before** the temp file is closed, before the rename, before the neighbor loop | `0x495f10` (clears flag +0xc0) |

Nothing is sent after the rename or the neighbor loop. Null45's sc4-auto-save subscribes to no save message (it calls `RequestSaveCity` from a timer). No published names for these ids. Load uses the same family: `0x26C63340` pre, `0x26C63341`/`0x26C63342` post (`0x4555f0`).

## 3. Order of operations in `0x455e20`

| # | Step | Address |
|---|---|---|
| 1 | Build paths. `0x91a497` creates a unique temp file `<dir><base><n>.tmp` (format `%s%s%d%s`, base defaults to `RZ_`, n counts down from `time()`), `CreateFileA` CREATE_NEW with share 0, then closes it. The directory was not traced; MoveFileA below needs the same volume, so it is almost certainly the region folder. | `0x455ff5` |
| 2 | New DBPF on the temp file, `Open(read=0, write=1)` | `0x4560c1` |
| 3 | Send PreSave, then `0x26C63346` | |
| 4 | Into the temp file: version record `0xEA3FF4F5` (`0x5dbfa0`), records `0x0B7D45E0..2`, `0x49DD6F4C/E`, then `0x4545d0`: city serialize (`0x5d99a0`, city = App+0x5c) and **own regional record** (`0x5dc1a0`, TGI `0xCA027EDB/0xCA027EE1`, record = App+0x58) | `0x4561b7`..`0x4562cf` |
| 5 | On success send `0x26C63344` and PostSave | |
| 6 | Close the temp DBPF (index written in Close `0x5d2170`) | `0x4563fd` |
| 7 | `0x454a10`: unregister and close the read handle this instance has held on its own `.sc4` since LoadCity (App+0x654) | `0x456449` |
| 8 | `SetCityNameChanged(false)`. If the target exists: delete a stale `target_`, `MoveFileA(target, target_)`, `MoveFileA(temp, target)`, delete `target_`. On failure delete the temp file and move `target_` back. Save-as cleanup deletes the old path. | `0x456458`..`0x4565ab` |
| 9 | `SetCitySaveFilePath`, then `0x454b20` reopens the target read-only and holds it again at App+0x654 | `0x45662f` |
| 10 | Only if the save succeeded: `NeighborsSim->GetAdjacentCities(list, true)`. For each neighbor with a save path (RC slot 0x7c), `0x5dc2e0` opens **that neighbor's `.sc4` in place**, checks version 0x3E..0x41, writes this process's copy of the neighbor's record (`0x5dc1a0`), then closes it | `0x456731`..`0x456793` |

So the own record is part of the own city file (step 4). The own file is complete and renamed into place (step 8) before any neighbor file is touched (step 10).

## 4. File APIs and share modes

The `cGZFile::Open(access, create, share)` wrapper is `0x919b00` → `CreateFileA`. access bit0 = GENERIC_READ, bit1 = GENERIC_WRITE. share: 1 = FILE_SHARE_READ, 2 = READ|WRITE, never FILE_SHARE_DELETE. create: 0..4 = CREATE_NEW, CREATE_ALWAYS, OPEN_EXISTING, OPEN_ALWAYS, TRUNCATE_EXISTING. DBPF `Open(r, w)` (`0x972a17`) always passes share 1.

| Handle | DBPF Open | CreateFileA | Held |
|---|---|---|---|
| Own `.sc4`, LoadCity `0x4555f0` / reopen `0x454b20` | (1,0) | GENERIC_READ, OPEN_EXISTING, FILE_SHARE_READ | Whole time a city is open, except steps 7 to 9 |
| Temp file | (0,1) | READ\|WRITE, OPEN_ALWAYS, FILE_SHARE_READ | Steps 2 to 6 |
| Neighbor `.sc4`, `0x5dc2e0` | (0,1) | READ\|WRITE, **OPEN_ALWAYS**, FILE_SHARE_READ | One record write |

What this means with two instances (A and B are adjacent, each with its own city open):

| Case | Result |
|---|---|
| B saves while A has its city open | B's write-open of `A.sc4` fails with a sharing violation because A's handle does not share write. `0x5dc2e0` returns 0 and nothing reports it. **A.sc4 is not touched.** |
| B saves while A is in region view or closed | B writes its copy of A's record into `A.sc4` (Q4 case a). |
| B opens `A.sc4` during A's step 7/8 before the first rename | A's `MoveFileA(target, target_)` fails (no FILE_SHARE_DELETE). A's save fails cleanly and the temp file is deleted. B's stale record lands in `A.sc4`. |
| B opens `A.sc4` between A's two renames | OPEN_ALWAYS **creates a new empty `A.sc4`**. A's `MoveFileA(temp, target)` and the restore both fail. The real city is left as `A.sc4_`, the new save is deleted, and `A.sc4` is a stub. Data loss. |
| B holds `A.sc4` during A's step 9 | A's read-only reopen fails, so App+0x654 is null until the next load. |
| A reads `B.sc4` (ReloadCity / ReloadAdjacent, region load) during B's step 8 | B's rename fails and B's save fails, or A sees no file. |

The windows are milliseconds wide, but the second case destroys data, so the lock is still needed. Two instances never write the same temp file, and a reader never sees a half-written own file (temp plus rename).

## 5. Other files

`0x455e20` itself touches only the temp file, the own `.sc4`, `<own>.sc4_`, a save-as old path, and adjacent neighbors' `.sc4`. It does not call region.ini, config.bmp or anything outside those paths. The region-view thumbnail data goes into the city DBPF (step 4 records). PreSave handlers (`0x43f1b0`, `0x45c1fe`, `0x6a9470`) were not traced for file I/O.

`0x5dc2e0` has one other caller: `0x4a7200`, a region-view operation (Region slot 0x2c/0x38 use, probably MoveCity). It is not part of the save.

## 6. Hook points for a lock

| Option | Covers | Notes |
|---|---|---|
| PreSave `0x26C63343` handler | Start of every save, before any file op | Synchronous. It cannot cancel the save, but it can block (wait for the lock). |
| PostSave `0x26C63345` handler | Runs before the rename and the neighbor loop | Too early to release. It is also not sent if serialization fails. |
| `MessagePost` of a private id from the PreSave handler | Runs at the next message pump after `0x455e20` and `RequestSaveCity` return | Release point without patching. Assumes the save does not pump posted messages (the progress/cursor helpers `0x5f8630`/`0x5f9320` are unverified). Needs one log check. |
| Patch vtable slot `0xa86b2c` with a wrapper (lock, call `0x455e20`, unlock) | Exactly the whole routine, every save path | Most precise. Needs VirtualProtect on `.rdata`. |

There is no message between "own file renamed" and "neighbor loop", so a plugin cannot use messages to lock only the neighbor writes.
