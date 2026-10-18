/*
 * sc4-region-sync
 *
 * TileLock.h - same-tile guard: a named mutex per open city,
 * Local\SC4RegionSync.Tile.<region directory name>.<x>_<z>, held by
 * existence (never waited on). Windows-only, no game headers. Main thread
 * only. See docs/PLAN-LAUNCHER.md A2.
 */

#pragma once
#include <cstdint>
#include <string>

namespace regionsync::tilelock
{
	// Creates the tile's mutex. True when another process already had it
	// (conflict); our handle is then closed at once so a later opener is
	// judged only against the live holder. False otherwise, including on
	// failure (logged).
	bool Open(const std::string& regionDirectoryName, int32_t x, int32_t z);
	// No-op when nothing is open.
	void Close();

	// After a conflicting Open: whether the mutex still exists (non-creating
	// open). Unknown errors read as present.
	bool PeerPresent();
}
