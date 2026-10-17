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

namespace regionsync
{
	// The standard RCI group IDs the demand simulator uses.
	constexpr std::array<uint32_t, 12> kRCIGroupIds
	{
		0x1010, 0x1020, 0x1030,          // R$, R$$, R$$$
		0x3110, 0x3120, 0x3130,          // Cs$, Cs$$, Cs$$$
		0x3320, 0x3330,                  // Co$$, Co$$$
		0x4100, 0x4200, 0x4300, 0x4400   // I-R, I-D, I-M, I-HT
	};

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
	};
	int ApplySnapshotToRegionalCity(const NeighborSnapshot& snapshot, cISC4RegionalCity* pRegionalCity, const ApplyOptions& options);

	// Convenience: find the regional record for a tile position, or null.
	cISC4RegionalCity* FindRegionalCity(cISC4Region* pRegion, int32_t regionX, int32_t regionZ);
}
