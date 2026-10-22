/*
 * sc4-region-sync
 *
 * RegionalCityBridge.h - moves data between the game's in-memory regional
 * city records (cISC4RegionalCity) and our plain NeighborSnapshot struct.
 *
 * Capture reads the game's record. Apply writes a snapshot from another
 * instance back into the record for that tile, which is what makes this
 * instance's demand and neighbor simulators see the other instance's city.
 */

#pragma once
#include "NeighborSnapshot.h"
#include <cstdint>
#include <array>

class cISC4RegionalCity;
class cISC4Region;
class cISC4City;
class cISC4Simulator;
class cISC4NeighborsSimulator;

namespace regionsync
{
	// Commodity slots written back into a record. The game keys commodity
	// stats by id in a map (any id works), but only 1..3 ever carry data.
	constexpr size_t kApplyCommoditySlots = 4;

	// Read the regional record for a tile into a snapshot.
	// pSimulator may be null; it is only used to stamp the sim date.
	void CaptureRegionalCity(cISC4RegionalCity* pRegionalCity, cISC4Simulator* pSimulator, NeighborSnapshot& out);

	// Write a snapshot into the regional record for a tile.
	// Returns the number of fields that changed value (0 means nothing new).
	// Only summary values are written. Deals and connections are never
	// touched here: see docs/DESIGN.md for why.
	struct ApplyOptions
	{
		bool established = true;
		bool budget = true;
		bool income = true;
		bool commodities = true;
		bool population = true;   // totals and per-RCI-group, direct field writes
		bool extrapolationCap = true;   // also set the record's +0x54 cap to the live value so the game stops extrapolating it (Q16)
		bool derivedDemand = false;     // also the 27 derived demand keys (SyncDerivedDemand, Q19)
	};
	// Per-key skips, summed over a pull: ids not carried, missing nodes per map (never inserted).
	struct ApplySkips
	{
		int notCarried = 0;
		int plain = 0;          // +0x3c
		int extrapolated = 0;   // +0x48
		int cap = 0;            // +0x54
	};
	int ApplySnapshotToRegionalCity(const NeighborSnapshot& snapshot, cISC4RegionalCity* pRegionalCity, const ApplyOptions& options, ApplySkips& skips);

	// Fill snapshot.connections from the own simulator's GetAllConnections.
	// Leaves connectionsKnown false when pNeighborsSim is null.
	void CaptureConnections(cISC4NeighborsSimulator* pNeighborsSim, NeighborSnapshot& out);

	// Fill snapshot.deals from the own simulator's live list (FindDealsAll).
	// pSimulator supplies the date objects for expiresIn; without it
	// expiresIn is 0. displayFields also reads amountUsed and cost (Dump only).
	void CaptureDeals(cISC4NeighborsSimulator* pNeighborsSim, cISC4Simulator* pSimulator, NeighborSnapshot& out, bool displayFields = false);

	// Convenience: find the regional record for a tile position, or null.
	cISC4RegionalCity* FindRegionalCity(cISC4Region* pRegion, int32_t regionX, int32_t regionZ);
}
