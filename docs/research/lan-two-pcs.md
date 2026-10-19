# Two PCs on one region: research summary (2026-10-19)

Research only, nothing built. Four lenses, each in `docs/research/lan/`:

| Doc | Lens | One-line result |
|---|---|---|
| `lan/audit.md` | Every same-host assumption in the plugin and launcher | 27 items, 6 blockers: both named mutexes, pid-only identity, host-gated liveness, per-host store default, local-only launcher rows. |
| `lan/game-network.md` | What players report about SC4 on network folders | Nobody has run SC4 live against a network Regions folder. The only working prior art copies region folders between turns (Syncthing, Dropbox). |
| `lan/smb.md` | SMB semantics our store would lean on | Rename is atomic across hosts and share modes are server-enforced, but a dead holder's lock can linger for minutes and listings lag up to 10 s. |
| `lan/save-design.md` | Save concurrency design study with the player's constraint applied | Host-authoritative replication through the launchers. No shared drive, no cross-host save lock. Three slices, 14 experiments (Q18). |

## Player constraint

No shared network drive. One PC hosts the region on its own disk; the other PC's launcher pairs with it, pulls the region, and pushes its saves back. Simple for the player even if the code is harder.

## Recommended design (from `lan/save-design.md`)

- Each PC keeps its own local Regions folder and local snapshot store. The game and the plugin never see a network path.
- The two launchers talk over one TCP connection (worker thread, one firewall prompt on the host the first time it hosts, none on the guest). Discovery by UDP broadcast or typed name; a 6-digit pairing code signs the channel.
- The launchers mirror the snapshot store and status files both ways. That alone gives live play across PCs: the plugin's publish and pull paths are unchanged.
- Every `.sc4` has exactly one owner. The host owns everything at rest. A guest takes a lease from the host launcher when it opens a tile; the lease is granted only if nobody has the tile open and the guest's copy matches the host's generation. The guest launcher pushes the file after every save; the host commits only if the push was based on its current generation, otherwise the file lands in `Conflicts\`. Leases apply only while connected. Both PCs may play alone while apart; on reconnect the launchers compare per-tile save counters (not file times, since the host's saves rewrite neighbor copies), fast-forward one-sided changes without asking, and show a per-tile picker for tiles changed on both sides. The losing copy is archived, never deleted. There is no Reclaim (section 10 of the study).
- No cross-host save lock. No file is shared between disks, so the save-641 races stay on one PC where the existing kernel mutex covers them. The launcher becomes a third, try-lock-only user of that mutex when it swaps files in.
- The game's neighbor write into a tile the other PC owns is ignored by the launcher (never pushed), and in a later slice suppressed by the plugin by holding that file open read-only during the save, the same silent failure vanilla already shows when the neighbor's city is open. Experiment 1 of Q18 decides whether that changes reload semantics for deals.

## Why not the alternatives

- Shared Regions folder on SMB: needs share, credentials and UNC setup; stretches the empty-file rename race across the network; Windows 11 24H2 blocks guest SMB access; no prior art. Rejected.
- Suppress neighbor writes everywhere and carry deals through the store: more divergence from vanilla than the problem needs. Used only where it removes a cross-PC write.

## Cost

Plugin: about 150 lines (save counter in the status file, lease file read, launcher-alive check, later the suppression). Launcher: about 2,000 lines on top of 2,155, with the manifest, lease and journal logic unit-testable on Linux.

## Next step

Write `docs/PLAN-LAN.md` for slice 1 (live only: discovery, pairing, handshake, first sync, snapshot and status mirroring) and run it through the plan review. Q18 experiment 1 (suppression keeps reload semantics) can run on one PC before any LAN code exists and decides slice 3.
