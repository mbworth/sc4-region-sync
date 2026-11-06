# Plan: two PCs on one region, slice 2 (saves flow both ways)

**Status:** reviewed 2026-11-05 (data integrity, security, simplicity); findings applied. Nothing built.

Inputs: `docs/PLAN-LAN.md` (slice 1, built; where this plan is silent, slice 1 holds), `docs/research/lan-two-pcs.md`, `docs/research/lan/save-design.md` (sections 2.4 to 2.9, 3, 10), `docs/research/save-641.md`, Q4, Q14, Q17 and Q18 in `docs/OPEN-QUESTIONS.md`.

Slice 2 is four increments, 2a to 2d, and all four are required before the public release (owner, 2026-11-05). Each ships on its own and keeps every slice 1 guarantee. Requirement "the two PCs cannot both save the same tile" is planned as: the second PC to open a tile goes dark (2b) and its saves are never committed over the other's; the plugin does not refuse the game's save. **Owner decision (2026-11-06):** dark saves stay local and the save is not refused. See D16 for the rule that follows from it.

**Today** (slice 1) the host's region is copied to the guest on connect, snapshots and status files mirror live, and guest saves stay on the guest flagged "not sent". A host save reaches the guest only at the next connect, and only when no game runs there.

## 0. Increments

Ordered by risk. Snapshots and status files mirror live throughout, as today.

| # | Adds | The player has after it |
|---|---|---|
| 2a | Closed-game sync: host journal, counters and per-tile `hostCount` in `MANIFEST`, guest push with host commit, host-to-guest delivery, one-sided changes resolved on reconnect, `Conflicts\` copies and flags for two-sided changes | Saves flow both ways. A save reaches the other PC once that PC has no SimCity 4 process running (and, on the saving PC, once the player has left that city). Until then the receiving launcher shows "swap pending, close SimCity 4". A tile saved on both sides is never overwritten: the guest's copy goes to `Conflicts\` and the tile is flagged; Re-sync keeps the host's. Same tile open on both PCs is still only a banner. |
| 2b | Cross-PC tile guard from mirrored status files | Opening a city that is open on the other PC gives a dark city, exactly as two instances on one PC do today. Both launchers name the tile in a banner. Two-sided changes are left to play while apart and to sub-second races. |
| 2c | Live swap: a not-open tile's file is replaced while games run on that PC; the saving PC stages right after the save | A save reaches the other PC within seconds while connected, unless that PC has the tile open. Gated on Q20 steps 18 to 21 (section 15). |
| 2d | Picker over the `Conflicts\` state 2a records | The player chooses per tile which copy to keep; the other stays in `Conflicts\`. Release candidate. |

## 1. Design-freeze gate

Before any slice 2 code, Q20 step 1 (section 15) runs on the current build: a tile's own save restored over a neighbor-written copy, both cities reloaded, deals and connections compared. Every rule below discards neighbor writes into the other PC's tiles (D1); slice 1 S8 already does so on connect, but nobody has checked a reload after it.

If it fails, D1 cannot stand: a tile's file would carry something the neighbor's load depends on. Slice 2 then stops before code. The likely rework is that each save carries its neighbor-written files and they commit only for tiles unchanged on the receiver (more conflicts, larger transfers); the plan is reviewed again.

## 2. Decisions

**D1. Only a lit counted save is a change.** A tile changed on a PC only when that PC's `_saves\<region>\<x>_<z>.saves` shows a save made lit in a city (`litCount` rose, P3) with `lastOk=1` from a writer on that PC. Everything else that changes a tile file is a neighbor write (save-641 step 10), observed 2026-11-05: one host save of BumperVille gave BumperVille, DumpsVille and TuggersVille the same mtime. Consequences:
- A hash change on the host without a lit save is host truth: delivered to the guest, never a conflict.
- A hash change on the guest without a lit save is an artifact: never pushed; the guest downloads the host's copy again.
- A push meets a destination that changed underneath it whenever a host save neighbor-wrote that tile, so the commit rule compares counters, never hashes (D3).

**D2. Which file belongs to which tile.** The `.saves` `file` is the file to push. The status file gains `cityFile` (P1). The launcher never reads a tile out of a `.sc4` (no parser exists, and none is added); it binds files to tiles:
- A host file is bound to at most one tile, by the host journal or the host `.saves`. Most host files start unbound (a region of 51 files may have a handful of `.saves`).
- The guest journal records, per tile, the base it last shared with the host: `baseFile` and `baseHash`. Today (`launcher/Journal.cpp` `UpdateJournalAfterSwap`) it records `file` and `baseHash` only for tiles named by the manifest's `tile`, the journal or a local `.saves`; slice 2 adds every other manifest `.sc4` as an unbound line `u.N=name|hash`. At the first counted save of a tile with no entry, the guest launcher takes `baseFile` from the `.saves` name the city was loaded from (`prevFile` if set, else `file`), only if that name is an unbound line, with its hash as `baseHash`; the entry then lives in the journal until `COMMITTED`, so it survives a reload. `prevFile` is not trusted by the host.
- A push binds an unbound host file to its tile under the rule in section 6. A file bound to nothing stays under hash rules: delivered when it changes, never pushed.

**D3. Commit rule.** The host commits a pushed file for tile t only if all hold, checked at precheck and again inside the lock right before the move (D6):
1. t is not host-changed (D7): the host `.saves` epoch equals the journal `hostEpoch` and its `litCount` equals `hostCount`;
2. 2a: no SimCity 4 process runs on the host. 2c: t is not open on the host (D15);
3. the push passes the validation in section 6 (name, count bounds, epoch, content);
4. the staged file's hash is still the one pushed, and no newer push for t arrived.

Otherwise the push is refused (`hostSaved`, `epoch`, `count`, `bad`, `space`) or held (`closed`, `open`, `lock`). A host dark save (P3) over no unsent lit save is not host-changed; its file is copied to the host's `Conflicts\` before the replace (D11).

**D4. Pushes coalesce.** Only the newest save of a tile exists on disk, so the push queue is "the latest lit save per tile".

**D5. Copies before the lock, moves inside it.** Any copy that can be large (staging, archive and conflict copies) happens before the save-mutex try-lock, with slice 1's verified method (file ID, size, mtime before and after; `Net.cpp` `StepStage`). Inside the lock the launcher only re-runs the decision, verifies that each source's file ID, size and mtime are unchanged, and renames. An original that is replaced in place is copied to `archive\` before the lock; a file that is removed (D8) is renamed into `archive\` inside the lock after its replacement is in place.

**D6. Every replacement re-decides inside the lock.** A delivery, commit or resolution that waited (pending, held, deferred by a modal dialog) runs its decision again inside the lock immediately before the move. A delivery is dropped if the local `.saves` epoch or `litCount` differs from the journal's base, or the journal hash for that tile moved since the `CHANGED` was accepted. When the host commits a pushed file it marks any staged older copy of that name gone, so a later `GET` for it gets `GONE`.

**D7. Counters and epochs.** Each `.saves` carries a random `epoch` (P3); both journals store the epoch next to every count. A count is compared only within one epoch. An epoch mismatch, or a count below the journal's base, means "changed here" (conflict path), never "artifact". Reading a slice 1 `.saves`: a missing `litCount` means `count`; a missing `epoch` means the fixed legacy epoch `0000000000000000`, which matches a journal entry with no epoch. The host journal's `guestEpoch` adopts the pushed epoch while it is legacy or empty, logged; every other epoch changes only on a commit, delivery or resolution.

**D8. One file per tile on the host.** Renames are decided by the tile's binding, never by `prevFile`. A rename is a push whose `name` differs from `baseFile`; on commit the host copies `baseFile` to `archive\` before the lock and, inside it, moves the new file into place and renames `baseFile` into `archive\`, so the tile ends with one file. Q20 step 8 and a unit test check the invariant.

**D9. Resolution never leaves a gap.** A copy that loses or waits goes into `Conflicts\` (copy, verified) before anything replaces it; the replacement is then a single `MoveFileExW(staging, target, REPLACE_EXISTING | WRITE_THROUGH)`.

**D10. Two-sided changes in 2a.** The host keeps its copy in its Regions. The guest copies its current file to `Conflicts\` with `conflict.txt` (tile, city, both counts and epochs, both save times, both hashes), keeps its file in its Regions, flags the tile "saved on both PCs, kept here", and stops pushing it. Re-sync keeps the host's for every flagged tile (D9). 2d adds the choice.

**D11. Dark saves.** A save in a dark city has `dark=1` (P3). It is never a change to sync and is always filed as the losing copy: the launcher copies that file into its `Conflicts\`, then the other PC's copy replaces it (guest: the host's current copy by `GET`; host: the guest pushes its current copy back, section 7.2). That replacement happens only when the dark save overwrote nothing unsent: on the host, `litCount` equals the journal `hostCount` (`hostState=dark`); on the guest, `litCount` equals `baseCount`. Otherwise a lit save that never reached the other PC was overwritten: the tile is a conflict (D10), both copies are kept, and a staged copy of that lit version that was never applied moves to that PC's `Conflicts\` instead of being deleted.

**D16. No sync while the region differs (owner, 2026-11-06).** The two PCs sync only when their copies of the region agree. Any discrepancy (a tile saved on both PCs, a dark save, an epoch mismatch) pauses all region sync for the pair: no record mirroring, no push, no delivery. Both launchers say so and offer the choice of master copy per differing tile (2d picker; before 2d, Re-sync, which keeps the host's). Sync resumes when every difference is resolved. Either PC can keep playing SimCity 4 meanwhile, unsynced. **Not yet applied below:** D10, D11, sections 7, 9, 10 and Q20 steps 13 to 15 still describe a per-tile flag with the rest of the region syncing, and a dark save losing without a prompt. They are to be amended and re-reviewed after the section 1 gate, before any 2a code.

**D12. Region layout comes from the host.** `region.ini` and `config.bmp` are never pushed. A guest-side change is copied to `Conflicts\` and replaced at the next no-game sync, with a banner.

**D13. 2b guard.** Each launcher creates and holds the plugin's tile mutex `Local\SC4RegionSync.Tile.<region>.<x>_<z>` for every tile that a mirrored peer status file shows in `city` state with `conflict=0` in the mirrored region, and closes it when that status leaves `city` state or the file goes away (slice 1's dead-writer reap and 10 minute disconnect rule). The plugin's existing guard (Q14) finds `peer=1` and the city goes dark: no publish, pull, mirror or plugin save, and it stays dark until closed. No lease protocol, no lease file, no plugin network code. A dead launcher drops its handles, so the guard fails open; D3 still prevents an overwrite.

Mutex names are case-sensitive. The plugin builds them as `"Local\\SC4RegionSync.Tile." + regionDirectoryName + "." + x + "_" + z` (and `"Local\\SC4RegionSync.SaveLock." + regionDirectoryName`), where `regionDirectoryName` is `cISC4Region::GetDirectoryName()` as the game returns it, widened with `CP_ACP` (`TileLock.cpp`, `SaveLock.cpp`). The launcher uses the same string from the same source: the `region` value of a local status file for that region (`AcpToUtf8(regionDirectoryName)`, converted back), else the on-disk folder name (`ExistingFolderName`, exact case, as slice 1's save mutex does). No case folding. Region names are printable ASCII (slice 1 name rules), so the code page conversion is the identity. A12 records that the game returns the on-disk case.

**D14. Mutexes across integrity levels.** The launcher creates the tile and save mutexes with an explicit security descriptor that a medium-integrity game can open: SDDL `D:(A;;0x1F0001;;;IU)(A;;0x1F0001;;;SY)S:(ML;;NW;;;ME)` (`MUTEX_ALL_ACCESS` for the interactive user and SYSTEM, medium label). The plugin creates its mutexes with the same descriptor (P4). The launcher treats `ERROR_ACCESS_DENIED` on either mutex as held by a game: no replace. An elevated launcher is allowed: games it starts run at its level, which worked in Q18 S10. It writes one Warning to `lan.log` at start when it runs elevated (`TokenElevation`). The descriptor is what fixes the mixed case (an elevated launcher beside a game started elsewhere). The launcher's error 740 fix (retry with `__COMPAT_LAYER=RunAsInvoker`, in the working tree, `launcher/Launch.cpp`) is observed on the GOG PC in Q20 step 2.

**D15. Open here.** A tile is open on a PC if a local status in city state names it (`regionX`, `regionZ`) or its file (`cityFile`, P1), its tile mutex exists and this launcher does not hold it (or opening it gives `ERROR_ACCESS_DENIED`), or a local snapshot with a live local writer names it (slice 1 `TileOpenHere`). 2a uses it for when the source may stage a file; 2c also for when a receiver may replace one.

## 3. Supersedes

| Where | Now | Becomes |
|---|---|---|
| PLAN-LAN 2 | "The save mutex `Local\SC4RegionSync.SaveLock.<region>` is used only by the guest's UI thread, only around the swap renames" | "...is used by either launcher's UI thread, only by try-lock, only around renames into Regions (sync swap, commit, delivery, resolution)." |
| PLAN-LAN 3 | "the data part carries the `file.N=name\|size\|hash\|tile` lines" | the section 6 format, `proto` 2 |
| PLAN-LAN 4 | "**Journal** (guest only)" | "Each side keeps a journal per peer and region (PLAN-LAN-2 section 8)." |
| PLAN-LAN 8 | "Slice 1's "the host's Regions folder is never written" is true of the launcher only." | "The host launcher writes a paired guest's city files into its Regions on a commit (PLAN-LAN-2 section 12)." |
| PLAN-LAN 9 | "The launcher writes into Regions only on the guest, and only under all of these conditions: ... only when no game process runs." | "The launcher writes into Regions on either PC, only under the CLAUDE.md ground rule as amended by PLAN-LAN-2." |
| PLAN-LAN 9 | "It never writes to the host's Regions. It never takes the save mutex anywhere else, so it can never stall a game save." | "On the host it writes only to commit a guest push or apply a resolution. It takes the save mutex only by try-lock around those renames, so it can never stall a game save." |
| PLAN-LAN 9 | "`NeighborSnapshot`, `InstanceStatus`, `Frame`, `Manifest` and `Journal` stay game-free and Windows-free." | adds `Ownership` |
| PLAN-LAN, slice 2 hooks | "`archive\` becomes `Conflicts\`." | "`archive\` keeps replaced originals; `Conflicts\` is added for copies of two-sided changes and dark saves." |
| PLAN-LAN, Not in slice 1 | "A cross-PC same-tile guard. Slice 1 has a banner and refuses the write." | 2b (D13) |
| CLAUDE.md ground rule | "The launcher may replace whole files in Regions only on the guest, from verified staging, originals archived first, under the save-mutex try-lock, with no game process running." | 2a: "The launcher may replace whole files in Regions only from verified staging, originals archived or copied to `Conflicts\` first, under the save-mutex try-lock, with no game process running on that PC. On the host it does so only for a commit or resolution that passes the commit rule in `docs/PLAN-LAN-2.md`." 2c adds, **provisional until Q20 steps 18 to 21 are observed:** "A single existing tile file may also be replaced while games run on that PC when none of them has that tile open, with the original copied out first and the replacement a single move. Adding, removing or renaming a file, or changing `region.ini` or `config.bmp`, still requires that no game process runs on that PC." |
| CLAUDE.md ground rule | "Never write to `.sc4` save files or the Regions folder from the plugin." | unchanged, plus "(The launcher's narrower exception is the ground rule above.)" |
| CLAUDE.md | Windows-free list `launcher/Frame`, `Handshake`, `Manifest`, `Journal` | adds `Ownership` |

## 4. Deviations from the study

From `docs/research/lan-two-pcs.md` and `docs/research/lan/save-design.md`:
- **Counters, not generations** (2.5). The two `_saves` counters plus the journals already order every played change per tile; a generation is a third clock that decides nothing new.
- **Latest save per tile, not an ordered queue** (2.7, "queued pushes in order"). Only the newest file exists on disk; older saves cannot be replayed.
- **Tile mutex from mirrored status, not leases** (2.4, `_lan\leases`, `launcher.alive`, the plugin dialog). The status files already cross; the plugin's own guard does the rest with no new plugin read path and no protocol.
- **Conflict copy kept on the PC that made it** (section 10, "archived under `Conflicts\` on both PCs"). One copy loses nothing; a second costs a transfer and disk.
- **Region layout always from the host** (section 10, "region layout" picker row). Layout edits while apart are rare, and two authorities over `config.bmp` would need a merge; the guest's copy is kept in `Conflicts\` (D12).
- **Rename under try-lock, not copy under the mutex** (2.6). A large copy under the lock can stall a save; slice 1's verified copy works without it (D5).

## 5. What it touches and what must not break

**Plugin** (no network code, no thread, no new game call):
- **P1.** Status key `cityFile=<basename>` while in city state, from `loadedSaveFile` (read at `PostCityInitComplete`). Schema stays 1.
- **P2.** The `Conflict:` line names the holder's host as well as its pid.
- **P3.** `.saves` gains `epoch` (16 lowercase hex from `BCryptGenRandom`, set when the file is created, or at the next save of a slice 1 file without one), `dark=1` when the save ran in a dark city (`conflict` set), and `litCount` (the count of the latest save made lit). `SaveCount` lines add `dark=1`. Older parsers ignore the keys.
- **P4.** `TileLock` and `SaveLock` create their mutexes with the D14 descriptor. `CreateMutexW` failing with `ERROR_ACCESS_DENIED` means "peer present", with a Warning: the tile guard goes dark (`TileLock: ... access denied, peer assumed`); the save lock retries the open at each `Acquire` within its existing timeout, then follows the timeout path (`unlocked=1`) and writes status key `lockDenied=1` for the process lifetime. The save is never refused (section 0). While any local status has `lockDenied=1`, the launcher copies and replaces nothing on that PC (banner `Sync paused: a game here cannot share the save lock. Close SimCity 4 and start it from the launcher.`). Today both fail open (`no same-tile guard`, `saves run unlocked`).

**Launcher, Windows-free** (unit-tested): `Frame.h` (new types), `Manifest.{h,cpp}` (manifest fields, push validation), `Journal.{h,cpp}` (epochs, host journal), new `Ownership.{h,cpp}`: tile classification, commit decision, delivery decision, guard set from statuses, picker suggestion. Inputs are plain structs.

**Launcher, Windows:** `Net.cpp` (region watch staging, push send and receive, `GET` by hash), `Lan.cpp` (journals, commit, delivery, `Conflicts\`, guard mutexes, live swap), `LanUi.cpp` and `launcher.rc` (picker `IDD_PICKER`, strip, banners).

**Must not break:**
- Without Host or Join nothing changes from today.
- The plugin never writes `.sc4` files or Regions; only the game saves cities.
- The launcher replaces files in Regions only under the amended ground rule (section 3), on either PC.
- The launcher takes the save mutex only by try-lock, held for milliseconds around renames (D5), so it never stalls a game save.
- Q4 save lock, Q14 tile guard, Q17 store watch and RegionWatch unchanged apart from P4. A launcher replace looks to them like a peer's save.
- Slice 1 first sync, mirroring, pairing, reaping and S9 handling unchanged; slice 1 frames keep their numbers and parse as before.
- `NeighborSnapshot`, `InstanceStatus`, `Frame`, `Handshake`, `Manifest`, `Journal`, `Ownership` stay Windows-free; `make test` and `make test-launcher` pass; `make launcher` has zero warnings.

## 6. Protocol

`proto` goes to 2 because the `MANIFEST` format changes. A slice 1 peer is refused at `HELLO` with `refused reason=proto` and the message `The other PC runs an older SC4 Region Sync (protocol 1, this PC 2). Install the same release on both PCs.`; the build check already forces equal builds. New frame types are appended to the enum. All slice 1 rules hold: exact counters, MAC, field formats, drop on any violation.

| Type | Direction | Fields | Receiver |
|---|---|---|---|
| `MANIFEST` (changed) | H to G | data lines `file.N=name\|size\|hash\|tile\|hostCount\|hostEpoch\|hostState`, the last three empty for unbound files; `info.x_z=count\|simDate\|population\|lastSaveUnix\|cityName` for each `changed` tile | section 7.1 |
| `HELLO` (changed) | both | adds `live=0\|1` (11.2) | the session is live only when both send 1 |
| `SYNCED` (changed) | G to H | adds `kept.N=x_z`, `conflict.N=x_z`, `resync=1` after a Re-sync | for every tile neither kept nor in conflict the guest now holds the announced file, and the host journal records its `hash`, `hostCount`, `hostEpoch` as announced in that `MANIFEST` (never the current values); kept and conflict tiles leave the host journal entry unchanged (conflict tiles only raise the host banner); `resync=1` clears every `guestCount` and `guestEpoch`, so the next push adopts them |
| `CHANGED` | H to G | `region`, `name`, `size`, `hash`, `tile`, `hostCount`, `hostEpoch`, `hostState`, or `layout=1` alone | 7.1 |
| `GET` (changed) | G to H | adds `hash` | host serves a staged copy with that hash, staging the current file on demand if its hash matches, else `GONE` |
| `GONE` | H to G | `region`, `name`, `hash` | the guest waits for the newer `CHANGED` |
| `APPLIED` | G to H | `region`, `name`, `hash` | only for a (name, hash) the host served by `GET` in this session; host journal: `hash`, `hostCount`, `hostEpoch` recorded with that staged copy |
| `PUSH` | G to H | `region`, `tile`, `name`, `baseFile` and `baseHash` (both empty for a founding), `size`, `hash`, `count`, `epoch` | precheck, then `READY` or `REJECTED` |
| `READY` | H to G | `region`, `tile` | the guest sends `FILE` |
| `FILE` (now both ways) | G to H | as slice 1; valid only after `READY` for the push in flight | host writes `staging\push\`, checks hash and content on `last` |
| `WAITING` | H to G | `region`, `tile`, `reason` (`closed`, `open`, `lock`) | strip only |
| `COMMITTED` | H to G | `region`, `tile`, `name`, `hash`, `count`, `epoch` | guest journal |
| `REJECTED` | H to G | `region`, `tile`, `reason` (`hostSaved`, `epoch`, `count`, `space`, `hash`, `bad`, `limit`, `bound`, `base`) | 7.2 |
| `RESOLVE` (2d) | G to H | `region`, `tile`, `hostCount`, `hostEpoch` (as shown), `count`, `epoch`, `hash` | keep the guest's copy, section 9 |
| `RESOLVED` (2d) | H to G | `region`, `tile`, `ok`, `reason` | section 9 |

`hostState`: `same` (hash changed without a host lit save), `changed` (host-changed, D7, including a dark save over an unsent lit one, D11), `dark` (the host's file is a dark save and `litCount` equals `hostCount`).

**Push validation** (host, at precheck; `Manifest.cpp`):
- `name` and `baseFile` pass `IsValidRegionFileName` and end in `.sc4`; `region.ini`, `config.bmp` and any other name are never valid. Names resolve case-insensitively against the host folder and always take the host's casing.
- **Bound tile** (the host journal or host `.saves` binds a file to it): `baseFile` must equal that file, else `REJECTED base`.
- **Unbound tile, `baseFile` set:** `baseFile` must be an existing host file bound to no tile (bound to another tile: `REJECTED bound`), and its current hash must equal `baseHash`, or the host journal's unbound line for it shows `baseHash` as the shared base (the file was only neighbor-written since) and D3 passes. Else `REJECTED base`. On commit the push binds the file (or its renamed successor) to the tile.
- **Founding** (`baseFile` empty): only when the tile is unbound on the host, the guest journal had no file for the tile at sync, and `name` matches no host file. Else `REJECTED base`.
- **Rename:** `name` differs from `baseFile` and matches no other host file (D8).
- Any `bound` or `base` refusal is shown on the guest as `<city>: <reason text>; kept here (Re-sync keeps the host's)` and takes the conflict path (D10), never an overwrite.
- `count` > journal `guestCount` and <= `guestCount` + 1000, `epoch` equal to the journal `guestEpoch` (adopted when empty). Over the bound: `REJECTED count`, banner "push refused: use Re-sync"; Re-sync resets `guestCount` (`SYNCED resync=1`). Epoch mismatch: `REJECTED epoch`, which the guest treats as a conflict. Exception: a tile the host announced `dark` accepts `guestCount` <= `count` <= `guestCount` + 1000 (D11).
- Content on `last`: hash equal, the first four bytes `DBPF`, size between 96 bytes and 1 GiB (`kMaxManifestFileSize`). The same content check applies to every `.sc4` the guest receives.

**Limits and checks** (S9 rules extended):
- `region` equals the mirrored region (`EqualsNoCase`), else drop. `tile` canonical (`ParseTileKey`) and inside `config.bmp` bounds.
- `size` <= 1 GiB; `hash` 64 lowercase hex; `count`, `hostCount` canonical decimal, u32; `epoch` 16 lowercase hex; `hostState` and `reason` from the fixed lists.
- `info` `cityName`: at most 255 bytes, printable ASCII only (anything else shown as `?`); `simDate` canonical decimal, shown through the launcher's own date formatting; `population` clamped as snapshot values (`5f10d44`).
- One push in flight per connection; a `PUSH` for the tile in flight replaces it (older staging deleted); another tile's `PUSH` before the first ends: drop.
- State checks, drop and log: `READY`, `COMMITTED`, `WAITING`, `REJECTED` for a tile with no push in flight; `FILE` G to H without `READY`; `APPLIED` for a (name, hash) not served by `GET` in this session; `RESOLVE` for a tile not in the conflict set.
- Replay: session keys and exact counters as slice 1; across sessions the count rules above, and `RESOLVE` must echo the `hostCount` and `hostEpoch` the host announced.
- The worker interleaves `FILE` chunks of both directions with mirror and `PING` frames, one chunk per transfer per loop, so a 1 GiB transfer cannot starve the 20 s liveness timer.

## 7. State machines

### 7.1 Host to guest delivery (2a)

Host:
1. **Detect.** A change handle on `Regions\<region>` plus the existing `_saves\<region>` handle, 1 s debounce.
2. **Stage.** A changed file is staged (D5) when its tile is not open on the host (D15), even while other games run there (A7, Q20 step 12). The city the host just saved is therefore staged after the player leaves it (2c lifts this). Each staged copy records the host `litCount` and epoch at staging. The connect-time `MANIFEST` staging stays as slice 1.
3. **Classify** (`Ownership`): hash equals the journal: nothing. A name added or removed, or `region.ini` or `config.bmp` changed: `CHANGED layout=1`. Else `CHANGED` with `hostState`.
4. **Serve.** Keep the staged copy per announced (name, hash) until `APPLIED`, a newer `CHANGED` of that name, a commit of that name (D6), or disconnect. A staged lit version superseded by a dark save follows D11.

Guest, on `MANIFEST` diff or `CHANGED` for a tiled file (`Ownership::DecideDelivery`):

| Guest state of the tile | Action |
|---|---|
| guest-changed (D7) and `hostState=changed` | conflict (D10) |
| guest-changed, `hostState` `same` or `dark` | no delivery; the push supersedes it |
| latest save dark (D11) | copy to `Conflicts\`, then deliver |
| any other | `GET`, verify into staging, then pending |

A pending delivery is swapped when no SimCity 4 process runs on the guest (slice 1 swap path and its "swap pending, close SimCity 4" state), re-decided inside the lock (D6), original copied to `archive\` before the lock (D5), single move. Then journal (`file`, `baseHash`, `baseCount` = local `litCount`, `epoch`) and `APPLIED`. Unbound files skip the first three rows. `layout=1` runs the slice 1 `SELECT` sync under the same no-game condition.

**Guest neighbor writes:** a tiled file whose hash differs from `baseHash` with no lit save is fetched again with `GET hash=<journal baseHash>`; a `GONE` means the host moved on and its `CHANGED` follows.

### 7.2 Push and commit (2a)

Guest, per tile:
```
idle --(lit save: litCount > baseCount, count == litCount, dark=0, lastOk=1, writer ours, not open here)--> stage (D5)
stage --(verified, .saves unchanged)--> PUSH        (.saves changed: stage again)
PUSH --READY--> FILE --> waiting --COMMITTED--> idle (journal)
                            \--WAITING--> waiting (strip: "waiting for <host>: close SimCity 4 there")
                            \--REJECTED hostSaved|epoch--> conflict (D10)
                            \--REJECTED space|hash--> retry in 60 s, then 5 min; banner
    --REJECTED count|bad|limit--> stop for the tile, banner, Error in lan.log
any --disconnect--> idle (pushed again after the next SYNCED)
```
- `lastOk=0`, or `lastOk` missing while the writer is dead: not pushed; "save failed here, not sent".
- Tiles in the writer's `saveCountFailed` list: never pushed (slice 1).
- `litCount > baseCount` with `count > litCount` (a dark save over an unsent lit one): never pushed, conflict path (D11).
- For a tile announced `hostState=dark`, the guest pushes its current file even when unchanged (D11).
- `REJECTED bound` or `base`: conflict path with the section 6 message.

Host:
1. Precheck: section 6 validation, free space (11.1), D3 1. Else `REJECTED`. Then `READY`.
2. `FILE` into `staging\push\fNNNN.part`; hash and content on `last`.
3. Idempotent re-push (hash equal to the host file): journal and `COMMITTED`, no move.
4. Wait for D3 2 (`WAITING closed`, strip "commit pending, close SimCity 4"). Then, before the lock: copy the original to `archive\` (or a dark host file to `Conflicts\`, D11), verified (D5).
5. Try-lock, re-run D3 (D6), verify the original and staging unchanged, single move of staging into place, rename every other file of the tile into `archive\` (D8), release. Failure: keep staging, `WAITING lock`, retry on the timer.
6. Host journal through `WriteAtomic` (`hash`, `guestCount`, `guestEpoch`, `file`; `hostCount` = host `litCount`). On failure: Error, banner, no `COMMITTED` (step 3 finishes it on the re-push).
7. `COMMITTED`; mark older staged copies of that name gone.

### 7.3 Reconnect and upgrade

On connect the `MANIFEST` diff applies 7.1 and 7.2 to every tile: one-sided changes are delivered or pushed without asking, two-sided changes are flagged (D10). Slice 1 S8's offline case becomes a push.

**First connect after upgrading from slice 1.** The host journal does not exist, so every tile with a host `.saves` count > 0 is host-changed (section 8). A tile the guest holds "not sent" from slice 1 and the host has ever saved therefore becomes a conflict: the guest's copy goes to `Conflicts\`, the tile stays in the guest's Regions flagged "saved on both PCs, kept here", and the host's file is untouched. A not-sent tile the host never saved is pushed. Nothing is overwritten on either PC. The banner reads `N cities saved on both PCs are kept here, not sent (Re-sync keeps the host's; copies in RegionSyncLAN\Conflicts)`.

### 7.4 Cross-PC tile guard (2b)

D13 on both launchers, on the 250 ms timer after each status mirror change. Lines: `Guard: holding tile (x,z) for <peer>`, `Guard: released tile (x,z)`. A race where both PCs open one tile within the mirror latency leaves both lit; both launchers show `BumperVille (4,6) is open on both PCs` and the counters turn the later push into a conflict. When the second opener's local status has `conflict=1`, its launcher shows `BumperVille (4,6) is open on <peer>: this copy is dark, its saves will not be sent` and the other shows `BumperVille (4,6) is also open on <peer> (dark there)`.

### 7.5 Live swap (2c)

2c replaces the "no SimCity 4 process" wait in 7.1 and 7.2 step 4 with "the tile is not open here" (D15), for one existing tile file only, and lets the saving PC stage right after `lastOk=1`. Sequence: copy the original to `archive\` (D5), try-lock, re-check open-here and re-decide (D6), verify, single move, release. A city that loads in between holds its file with read sharing only, so the move fails with error 32 (A3) and is retried; after 20 failures it waits for that tile's status to change.

| Situation on the receiving PC | Replace? | Basis |
|---|---|---|
| tile open in a city there | never | rule above; A3 backstop |
| tile adjacent to a city open there | yes | a local peer's save does the same (Q4, Q15); A2 |
| tile in a region view there | yes | Q17 step 4 (peer save); A4 |
| a city loading there | only before its `LoadCity` open | A3, A5 |
| a save in progress there | no: the try-lock fails | Q4 |
| rename, new or removed file, `region.ini`, `config.bmp` | only with no game process | slice 1 path |

## 8. Journals and counters

**Guest** `journal\<hostSiteId>\<region>.txt`: `x_z=baseFile|baseHash|baseCount|epoch` per tile, plus `u.N=name|hash` for each manifest `.sc4` bound to no tile (D2).
- Guest-changed: `litCount > baseCount`, an epoch mismatch, a count below `baseCount`, or `saveCountFailed` (D7).
- Set after a swap or delivery (`baseCount` = local `litCount`), after `COMMITTED` (`baseFile` = the committed name, `baseHash`, `baseCount` = pushed count; its `u.N` line removed), after a resolution.

**Host** `journal\<guestSiteId>\<region>.host.txt`, new: `x_z=file|hash|hostCount|hostEpoch|guestCount|guestEpoch` per bound tile, plus `u.N=name|hash` for each unbound `.sc4` with the hash both PCs held at the last `SYNCED` or `APPLIED` (the shared base).
- `file`, `hash`: the version both PCs hold. `hostCount`, `hostEpoch`: the host save count of that version, as announced in the `MANIFEST` or `CHANGED` that `SYNCED` or `APPLIED` answered. `guestCount`, `guestEpoch`: the last commit.

`Journal.cpp` gets `BuildHostJournalText` and `ParseHostJournalText` with the same strict rules. Written with `WriteAtomic` after every change; a failed write is an Error and a banner.

**Missing or unreadable.** Host: every tile with a host `.saves` count > 0 is host-changed, so a guest-changed tile becomes a conflict, never an overwrite; banner `host journal missing: tiles saved on both PCs are kept apart`. Guest: slice 1's unknown-folder path stays.

**Clocks** are never compared across PCs for decisions; `lastSaveUnix` is display only.

## 9. Picker (2d)

**When.** On the guest, after the `MANIFEST` diff and whenever a tile is flagged, once neither PC has that tile open. Slice 1's modal rules apply. The host shows a banner.

**Rules:**
- One row per flagged tile; layout is never a row (D12).
- The suggestion is the later sim date as each PC reports it (a tie suggests the host), labelled `suggested from the dates each PC reports`.
- **Keep host:** the guest copies its current file to `Conflicts\` if it differs from the copy already there, then the host's copy replaces it (7.1, D9).
- **Keep guest:** `RESOLVE` with the shown `hostCount` and `hostEpoch`. If the host saved since: `RESOLVED ok=0 reason=changed`, asked again. Otherwise the host copies its file to its `Conflicts\`, adopts the pushed `epoch` and `count` as `guestEpoch` and `guestCount`, and accepts the guest's `PUSH` for that count without D3 1. The `PUSH` that follows passes the section 6 binding rule as usual.
- **Decide later:** rows stay flagged, the banner stays.
- Re-sync becomes "Keep all from host" for every flagged and not-sent tile.
- At most 3 resolutions per tile per session (`REJECTED limit`).

**Wording.** Title `Cities saved on both PCs`. Text `These cities were saved on both <host> and <guest> since they last synced. Choose which copy to keep. The other copy stays in RegionSyncLAN\Conflicts on the PC that saved it; nothing is deleted.` Columns `City`, `Tile`, `<host>`, `<guest>`, `Keep`; cell `pop 50,037, Jun 2153, saved 14:02 (3 saves)`, tooltip `(that PC's clock)`. Buttons `Keep all from <host>`, `Keep all from <guest>`, `OK`, `Decide later`. Guest banner `2 cities saved on both PCs: choose in Resolve`; host banner `2 cities wait for a choice on <guest>`.

## 10. Failure handling

| Case | Outcome |
|---|---|
| Disconnect mid-push or mid-delivery | The receiver deletes the part. The guest pushes again after the next `SYNCED`; the `MANIFEST` diff carries deliveries. |
| Host crash after the move, before `COMMITTED` | The file is in place, the journal old. The host sees a hash change without a lit save (`same`). The guest re-pushes; the idempotent path journals it. |
| Host crash before the move | Staging deleted at start; the guest re-pushes. |
| Crash mid swap (2a) | Slice 1 rollback. 2c: the target is old or new, never missing (D9). |
| Both save one tile (2a, or a 2b race) | `REJECTED hostSaved`; D10. |
| Rename or founding | Waits for no game on the receiving PC; D8 keeps one file per tile. |
| Push base or binding mismatch | `REJECTED base` or `bound`, conflict path with the reason shown (section 6). |
| Disk full or caps reached | 11.1: sync pauses on that PC with a banner naming the folder. Nothing is replaced without its copy, nothing is dropped. |
| `lastOk=0`, `saveCountFailed` | Never pushed or replaced on that PC; flagged. |
| Epoch reset (store deleted, new `StoreDirectory`) | Changed here (D7): pushed if the other side is unchanged, else a conflict. |
| Dark save on either PC | D11. |
| Guest edits region layout | D12. |
| Mixed elevation | The tile guard fails closed (dark city, Warning). The save lock degrades: after its timeout the game saves unlocked with a Warning and `lockDenied=1`, and the launcher then copies and replaces nothing on that PC (P4). An elevated launcher only logs a Warning (D14). |
| `LiveSync=0` | 11.2. |

## 11. Disk, settings and UI

### 11.1 Disk

- Free space >= size + 10 % + 256 MiB before any staging, archive copy or conflict copy, on both PCs. Otherwise `REJECTED space` (push) or the delivery waits, with a banner.
- Copies made for a peer go under `archive\<peerSiteId>\` and `Conflicts\<peerSiteId>\`, each capped at 8 GiB per peer.
- No copy is pruned by count. A copy that exists nowhere else (every `Conflicts\` copy, and an archived version the journals never show as held by both PCs) is kept until the player deletes it. An archived version whose hash the journal shows both PCs hold or held as a shared base, and archived neighbor-written files, are deleted after 14 days.
- At the cap, or below the free-space floor, sync pauses on that PC: no staging, copy or replace, pushes get `REJECTED space`, deliveries wait, and the banner reads `Sync paused: RegionSyncLAN is full. Delete old copies in <full path of the folder>`. Nothing is dropped; sync resumes once space is freed.
- At most 3 resolutions per tile per session (section 9).

### 11.2 Settings

- `[Lan] LiveSync=1`. `LiveSync=0` on either PC, exchanged in `HELLO`, gives slice 1 behaviour for the session on both: no `CHANGED`, `PUSH` or `RESOLVE`; the host journal is neither read nor written; the guard holds no mutex; guest saves stay "not sent" and the guest journal is updated only by the slice 1 swap. A stale host journal later only makes more tiles host-changed, never fewer.
- Downgrade to a slice 1 build: it ignores `*.host.txt`; its strict parser rejects the five-field guest journal and treats it as empty, so every tile the guest ever saved shows "not sent" and is kept. Nothing is overwritten. `.saves` keys added by P3 are ignored and dropped at the next slice 1 save, which reads as an epoch change after re-upgrading (D7, conflict path).
- Test-only, undocumented, in the shipped build: `DebugCommitPauseMs=0` (pause between move and `COMMITTED`) and `FreeSpaceFloorMiB=256`.

### 11.3 UI

- **Strip:** guest `pushing TuggersVille 4/18 MiB`, `TuggersVille sent 14:02:11`, `waiting for <host>: close SimCity 4 there` (2a) or `BumperVille open there` (2c); host `BumperVille sent to <guest> 14:03:40`, `commit pending, close SimCity 4`. The tooltip lists pending, waiting and flagged tiles.
- **Banners added:** saved on both PCs; swap or commit pending; tile open on both PCs, naming the tile (2b); host journal missing; push refused; save failed here; RegionSyncLAN full; access denied on a mutex.
- **Rows:** no new toolbar button until 2d adds `Resolve` (guest, enabled when tiles are flagged).

## 12. Trust model

Slice 1 pairing trusted the host with the guest's game. From 2a, pairing also lets the guest write city files into the host's Regions: a paired guest can replace any host city file the host has not changed since the shared base. That is what sync means. It cannot create case-variant duplicates, touch `region.ini`, `config.bmp` or non-`.sc4` names, bind one file to two tiles, overwrite a host-changed tile, push past the count bound, or skip the archive copy and disk caps of 11.1; every pushed file must carry the `DBPF` magic and fit the size cap. The host cannot check that a pushed file is a sound city or belongs to the tile it claims, because the launcher has no `.sc4` parser and slice 2 adds none; a malicious paired guest can hand the host's 2003 loader a hostile file, as a malicious host already can the guest. The paired peer is the player's own PC, and pairing is the trust boundary. README and `docs/DESIGN.md` must say this in plain words.

## 13. Operator steps

1. Create `## Q20` in `docs/OPEN-QUESTIONS.md` with the steps of section 15 and empty Observed slots, before any work starts.
2. Run Q20 step 1 on the current build (design-freeze gate). Step 2 (740 fix, elevation) is the first step of 2a.
3. Build once and deploy the same build to both PCs (`make deploy`, then copy the dist to the second PC); the build and `proto` checks refuse mixed builds.
4. Back up `Regions\<region>` on both PCs.
5. First connect after the upgrade: close SimCity 4 on both PCs (7.3).
6. On completion of each increment, update the docs in section 17.

## 14. Tests (`tests/launcher_lan.cpp`)

- Frames: every new type round trip; unchanged slice 1 frames still parse; drop rules for out-of-state frames, `FILE` G to H without `READY`, a second push, `APPLIED` without its `CHANGED`, bad `reason` or `hostState`; a `proto` 1 `HELLO` refused with the section 6 message.
- Fields: tile bounds, `count` canonical and u32, `epoch` format, `cityName` ASCII and cap, `simDate` format, the `|` split.
- Push validation: each section 6 rule alone, including `region.ini`, `config.bmp`, a non-`.sc4` name, case-only matches resolving to the host's casing, a rename onto an existing name; binding: bound tile with matching and wrong `baseFile`, unbound tile binding an unbound file (hash equal, and hash moved by a neighbor write with the shared base matching), `baseFile` bound to another tile (`bound`), founding with the tile bound, with a guest base, and with a colliding name; count bounds (equal, +1, +1000, +1001), the `dark` exception; `DBPF` magic and size bounds.
- `MANIFEST` with the new columns and `info` lines; a slice 1 line is refused.
- Journals: guest and host round trip including `u.N` lines, strict parse, a slice 1 guest line adopting an epoch, a missing host journal making saved tiles host-changed.
- Push trigger: `count > litCount` (dark over unsent lit) goes to conflict; `hostState=dark` only when `litCount == hostCount`; `APPLIED` for a hash served by an on-demand `GET`; an accepted `RESOLVE` adopting the epoch; the guest's first-save binding from `.saves` (`prevFile` or `file` matching an unbound line, and not matching); legacy `.saves` read (no `litCount`, no `epoch`).
- Pruning: an archived version held by both PCs is prunable by age, one that exists nowhere else is not; the cap pauses sync.
- `DecideCommit`: each D3 condition alone; idempotent re-push; rename and founding with D8 archiving every other file of the tile; epoch mismatch on either side; count below base; a host neighbor write between base and push (must commit); a host dark save (commits after the conflict copy).
- `DecideDelivery`: each 7.1 row; a tile with `cityFile` set in a status but no counted save (open here, pending); re-decision inside the lock dropping on a moved count, epoch or journal hash (D6); a dark latest save.
- Guard set (`Ownership`): peer `city` with `conflict=0` held, `conflict=1` not held, status gone released, wrong region ignored.
- Picker suggestion; `RESOLVE` with a stale `hostCount` or `hostEpoch` refused; the resolution limit.
- `ParseInstanceStatus` with `cityFile` and `lockDenied`; `ParseSaveCounter` with `epoch`, `dark`, `litCount`, and without them.

Windows-only, tested by hand in Q20: change handles, staging and verify, mutex descriptors and access-denied paths, swaps and moves, free-space checks, the picker dialog.

## 15. Experiment: Q20

Region New York: BumperVille (4,6), TuggersVille (6,6), DumpsVille (4,4). Back up both region folders. Hashes as in Q18. Loopback (one box, `-LoopbackGuest 2`) first, then two PCs. Loopback limits: both sides share one session, so tile and save mutexes are shared and "no SimCity 4 process on that PC" sees every instance; steps marked **[2 PCs]** need two PCs. `lan.log` lines named below are added by this plan; `H8` is the first 8 hex of a hash. Timing: loopback figure first, two PCs (wired, files up to 50 MiB) second. No step uses a build that skips a check.

**Gate** (the only step on the current build, before any slice 2 code):

1. **Discarded neighbor write keeps deals (section 1).** One PC, two instances, a water deal across the TuggersVille-BumperVille edge. Save TuggersVille, copy its file aside. Save BumperVille (neighbor-writes TuggersVille). `RegionSyncDump` in both. Exit both; with no game running, copy the saved-aside TuggersVille file back over the neighbor-written one from PowerShell. Reopen both, `RegionSyncDump`.
   - Pass: the deal appears once on each side with the pre-exit amounts, and connection counts equal the pre-exit dump.
   - Fail: a missing or doubled deal or connection. Slice 2 stops (section 1).

**2a (loopback unless marked):**

2. **740 fix and mixed elevation [2 PCs, GOG guest].** (a) The non-elevated launcher launches the GOG instance. Pass: the instance starts and logs `SaveLock: ... opened`. Fail: the 740 message box. (b) Start the launcher elevated: one Warning in `lan.log`, Host and Join work, and games it starts sync (`Deliver` and `Commit` as in steps 3 and 4). (c) From an elevated PowerShell in the player's session, create `Local\SC4RegionSync.SaveLock.New York` with a default `[System.Threading.Mutex]`, then open TuggersVille in a medium game and press Ctrl+S. Pass: Warning `SaveLock: ... access denied`, the save `ok=1 unlocked=1`, status `lockDenied=1`, and `lan.log` `Sync paused: save lock unavailable`; no `Deliver` or `Commit` until that game exits. Fail: `CreateMutexW ... failed, error=5; saves run unlocked` with no Warning, or any copy or replace while paused.
3. **Host save delivered closed.** Instance 1 saves BumperVille, exits to the region view, then quits. Pass: within 5 s / 15 s of the last game exit, `Deliver (4,6) ... applied` plus one per neighbor-written tile (`hostState=same`), equal hashes, host journal `hostCount` equal to the announced count, and `region.ini` hashes equal before and after the exit (A9). Fail: any `Deliver ... applied` while a game runs on the guest, unequal hashes, or `CHANGED layout=1`.
4. **Guest push committed closed.** Instance 2 saves TuggersVille and quits; instance 1 not running. Pass: within 5 s / 15 s, `Commit (6,6) ... committed count=N`, equal hashes; on reopening on the host, `RegionSyncDump` equals the guest's. The guest's neighbor-written BumperVille is fetched again (`Refetch (4,6)`), never pushed. Fail: `REJECTED`, a push of (4,6), or unequal dumps.
5. **No echo after commit.** After step 4, open and exit TuggersVille on the guest without saving, wait 60 s. Pass: no `Deliver (6,6)` line, hashes equal on both PCs. Fail: any delivery of (6,6).
6. **Host neighbor write under a guest change.** The guest saves TuggersVille; before it commits, the host saves BumperVille (rewriting the host's TuggersVille with no lit save). Pass: `Commit (6,6) ... committed`. Fail: `REJECTED hostSaved`.
7. **Both save one tile.** Both save TuggersVille while disconnected, then Join. Pass: `Conflict (6,6) copy -> Conflicts\...`, `conflict.txt` present, host file and guest file hashes unchanged, banner shown. Fail: any `Deliver (6,6)` or `Commit (6,6)`. Variant: a city the host never saved with the plugin, saved on the guest only: `Commit (6,6) ... bound` (the push binds the host file), one file for (6,6). Variant: the guest's `baseFile` bound to another tile on the host: `REJECTED bound`, conflict path, no file changed.
8. **Rename, one file per tile.** (a) Rename TuggersVille on the guest and save; before the commit, reopen the renamed city on the guest. Pass: the guest log shows `SaveCount: (6,6) loaded from '<new name>'`, the guest journal keeps `baseFile` as the old name until `COMMITTED`; after commit the host holds exactly one `.sc4` for (6,6) (by `.saves` and a directory listing), and the old name is in `archive\`. Fail: two `.sc4` files for (6,6) on the host. (b) The same on the host toward the guest.
9. **Disconnect mid-transfer.** A PowerShell loop tails `lan.log` (`Get-Content -Wait | Select-String 'FILE .* offset='`) and runs `Stop-Process` on the host launcher at the first progress line, once during a push and once during a delivery. Pass: parts deleted, the transfer redone after reconnect, equal hashes. Fail: a part in Regions or unequal hashes.
10. **Crash mid-commit.** `DebugCommitPauseMs=10000`; kill the host launcher in the pause. Pass: no file missing; at restart the re-push logs `Commit (6,6) idempotent`. Fail: a missing file or a second move.
11. **Disk, hostile input, failed save.** (a) `FreeSpaceFloorMiB` above the host's free space: `REJECTED space`, the `Sync paused` banner naming the folder, nothing in Regions. (b) S9's three unauthenticated cases again; authenticated hostile frames are covered by `make test-launcher` (the receiver calls the same validators). (c) Hold `City - TuggersVille.sc4` open on the guest from PowerShell (`[IO.File]::Open(..., 'Open', 'Read', 'Read')`) and press Ctrl+S: `SaveCount ... ok=0`, no `Push (6,6)`, "save failed here". Fail: any file written, or a push.
12. **Staging beside a running game (A7).** Instance 1 in BumperVille on the host saves five times about 30 s apart while the host launcher stages the neighbor-written TuggersVille and DumpsVille after each. Pass: five `ok=1`, no `SaveLock` timeout, every staged hash equal to the file after the copy; record the number of `Stage: retry` lines and of saves after which a neighbor's mtime did not change (each is a skipped neighbor write). Fail: any `ok=0`, a timeout, or `Stage: retry exhausted`.

**2b [2 PCs]:**

13. **Guard.** The host is in BumperVille; the guest opens it. Pass: within 2 s of the host status landing on the guest, `Guard: holding tile (4,6) for <host>` in the guest's `lan.log`, then the guest plugin `TileLock: ... already exists, peer=1`, the `Conflict:` line names the host, both banners name the tile. The guest's Ctrl+S gives `dark=1` and never a `Commit (4,6)`. Reverse roles. Fail: a lit second opener or any commit of a dark save.
14. **Guard release.** End the guest game: `Guard: released tile` within 2 s of the reap. Kill the guest launcher: the host keeps the mutex 10 minutes (plus or minus 30 s), then releases. Fail: an early or missing release.
15. **Dark save on the host.** The guest opens TuggersVille; the host opens it (dark) and saves; the guest saves and exits. Pass: host `SaveCount: (6,6) ... dark=1`, `Conflict (6,6) dark copy -> Conflicts\...`, then `Commit (6,6)` and the guest's hash in the host's Regions. Variant: the host saves TuggersVille lit and exits, then, before the delivery, reopens it dark and saves: tile flagged, both copies kept, no `Commit (6,6)`. Fail: `Deliver (6,6)` of a dark file, or a dark file left in the host's Regions after the guest's push.
16. **Guard across elevation.** With the D14 descriptor, an elevated PowerShell creates the tile mutex for (6,6); a medium game opens TuggersVille. Pass: `TileLock: ... already exists, peer=1`, no Warning. With a default `[System.Threading.Mutex]` instead: Warning `TileLock: ... access denied, peer assumed`, the city dark. Fail: `CreateMutexW ... failed, error=5; no same-tile guard`.
17. **Mutex name case (A12).** Compare the `region=` value of a status file with the on-disk folder name on both PCs, and confirm step 13's guard with a region whose folder name has mixed case. Pass: byte-equal names and `peer=1`. Fail: a lit second opener.

**2c gate** (18 and 19 from PowerShell before 2c code; 20 and 21 on the 2c build before it ships; the 2c ground-rule wording stays provisional until all four pass):

18. **Copy right after a save (A1).** A PowerShell loop copies TuggersVille's file with `[IO.File]::Open(..., 'Open', 'Read', 'Read, Delete')` and hashes each copy, while the player presses Ctrl+S five times about 2 s apart. Pass: five `SaveCount ... ok=1`, no `SaveLock` timeout, and the copy taken after each `SaveLock: released` hashes equal to the file. Fail: any `ok=0`, a timeout, or a torn copy.
19. **Rename over an open city fails (A3).** From PowerShell, `[IO.File]::Move($copy, $target, $true)` over TuggersVille's file while instance 2 has it open. Pass: a sharing violation (error 32), hash unchanged. Fail: the move succeeds.
20. **Replace beside a running game (A2, A4).** Instance 2 in TuggersVille on the guest; the host saves DumpsVille, then BumperVille. Pass: both `Deliver ... applied` within 5 s / 15 s; no Error or Warning line in the guest's `SC4RegionSync.log` until its next `Pull`, and the game still running 60 s later. Then instance 2 exits to the region view and the host saves BumperVille again: `RegionWatch: N tile(s) due` and `Reload (4,6) ... recordPtrChanged=1` within 5 s / 15 s. Fail: a crash, a dialog, a `SaveLock` timeout.
21. **City load during a delivery (A5).** Click Play on BumperVille on the guest during a delivery of it. Pass: `SaveCount: (4,6) loaded from` and a `RegionSyncDump` population equal to the dump of either the old or the new file; any failed move logs `Deliver (4,6) retry error=32`, then `applied` within 15 s / 30 s of the city closing. Fail: a load error, a crash, or a dump matching neither file.

**2d and soak [2 PCs]:**

22. **Picker.** Both save TuggersVille while apart; Join. Pass: one row; Keep guest puts the host file in the host's `Conflicts\` and commits the guest's, equal hashes; Keep host does the reverse on the guest and ends with `APPLIED`; Decide later keeps the banner; a host save between opening and OK gives `RESOLVED ok=0 reason=changed`. After a guest epoch reset (rename the guest's `_saves\New York`), Keep guest commits once and the next push is accepted. Fail: a file replaced without its copy, or `REJECTED epoch` after an accepted `RESOLVE`.
23. **Latency.** Repeat 3, 4 and 7 wired. Record the time from `SaveLock: released` to `COMMITTED` or `APPLIED` with `w32tm`-synced clocks; 2a target 15 s after the receiver's last game exit, 2c target 15 s after the save.
24. **Soak.** 60 minutes, host in BumperVille, guest in TuggersVille, saves every few minutes, one exit to the region view and back each. Pass: every save committed or delivered, no conflicts, no Warning lines; `Stage: retry` lines counted and recorded (A7).

**Observed.** (empty, one slot per step)

## 16. Game behaviour assumed, not yet observed

| Increment | # | Assumption | Basis so far | Step |
|---|---|---|---|---|
| all | A0 | Discarding a neighbor-written file in favour of the tile's own save keeps deals and connections after reload | study 2.9 hypothesis; S8 drops them on connect; S10 saw a live deal only | 1 |
| 2a | A6 | A city saved on one PC loads identically on the other | S10 GOG guest and Steam host share vtables; no file compare yet | 4 |
| 2a | A7 | A staging copy beside a running game only skips a colliding neighbor write and never blocks a save | Q4 launcher note (theory); slice 1 stages at connect | 12, 24 |
| 2a | A8 | A failed save leaves `lastOk=0` and the previous file restored by the game | save-641 step 8 (Ghidra) | 11c |
| 2a | A9 | `region.ini` is rewritten on city exit with the same bytes, so no false `layout=1` | Q4 saw a write, bytes not compared | 3 |
| 2a, 2b | A11 | A medium-integrity game opens a mutex made with the D14 descriptor by an elevated process | Windows docs | 2, 16 |
| 2b | A10 | The plugin treats a launcher-held tile mutex like an instance's, with Q14's dark city | code reading (`TileLock::Open`) | 13 |
| 2b | A12 | `GetDirectoryName()` returns the region folder name in its on-disk case | code reading only | 17 |
| 2c | A1 | Copying a city's own `.sc4` right after its save never blocks or fails the next save | save-641 share modes (Ghidra) | 18 |
| 2c | A2 | Replacing a tile file not open in a running game is as safe as a local peer's save | Q4, Q15, Q17 observed peer saves, not a launcher move | 20 |
| 2c | A3 | A game with a city open holds its file with read sharing only, so a move over it fails with error 32 | save-641 (Ghidra); S4 saw error 32 from a PowerShell handle | 19 |
| 2c | A4 | RegionWatch reloads a launcher-replaced tile like a peer's save | Q17 step 4 (peer save) | 20 |
| 2c | A5 | A city load reads only its own file, held from `LoadCity`, and no other tile's `.sc4` after region init | unknown | 21 |

## 17. Docs to update on completion

- `release/README.txt`: saves flow both ways; when a save arrives (2a: other PC's games closed; 2c: live); `Conflicts\`; the trust paragraph (section 12); the uninstall line about where the launcher replaces files.
- `release/SIMTROPOLIS-POST.md`: the same, replacing "Guest saves stay on the guest in this release".
- `docs/DESIGN.md`: Two PCs section and trust model (section 12).
- `docs/ARCHITECTURE.md`: host journal, counters and epochs, commit and delivery, guard mutexes and descriptor, `Conflicts\`, source map (`Ownership`).
- `docs/PHASES.md`: split slice 2 into 2a to 2d checkboxes; the cross-PC same-tile guard moves from slice 3 to 2b.
- `CLAUDE.md`: status line, the amended ground rule (section 3, 2c part provisional until observed), the launcher exception next to "Never write to `.sc4` save files or the Regions folder from the plugin", `launcher/Ownership` in the Windows-free list.
- `docs/OPEN-QUESTIONS.md`: Q20 Observed slots; a Q4 note that the host launcher now try-locks the save mutex too.

## Not in slice 2

- More than one guest, more than one region per connection, launching instances on the other PC.
- Encryption, the Plugins diff list, IPv6, non-ASCII region names.
- Pushing region layout from the guest (D12).
- Live renames, new or removed files while games run.
- Reading a `.sc4` to check its tile or soundness: no parser exists; the paired peer is the player's own PC and pairing is the trust boundary (section 12).
- An in-game notice for a dark city; the launcher banner and the Q14 log line are the signal, as on one PC.
- Refusing a save in the plugin (owner decision pending, section 0).
- Suppression of foreign neighbor writes by the plugin (study 2.9; slice 3).
- A cross-PC save lock; the copy under the save mutex (D5), unless step 24 calls for it.
- The tile picture refresh after a replace (Q17, parked).
