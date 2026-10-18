/*
 * sc4-region-sync
 *
 * NeighborMirror.h - creates, in this instance, the connections and deals
 * another instance published (docs/PLAN-PHASE2.md, slice 1: create only).
 * Game-facing like the bridge; never touches the message system.
 */

#pragma once
#include "NeighborSnapshot.h"
#include <cstdint>
#include <set>
#include <tuple>

class cISC4NeighborsSimulator;
class cISC4Region;
class cISC4RegionalCity;
class cISC4Simulator;

namespace regionsync
{
	struct MirrorContext
	{
		cISC4Region* pRegion = nullptr;
		cISC4RegionalCity* pOwnRegionalCity = nullptr;
		cISC4NeighborsSimulator* pNeighborsSim = nullptr;
		cISC4Simulator* pSimulator = nullptr;
	};

	class NeighborMirror
	{
	public:
		// Each returns the number of game objects created.
		int MirrorConnections(const MirrorContext& ctx, const NeighborSnapshot& publisher);
		int MirrorDeals(const MirrorContext& ctx, const NeighborSnapshot& publisher);

		// Call when the city closes; the sets are per city session.
		void Reset();

	private:
		// (publisherX, publisherZ, type, innerX, innerZ), publisher's frame.
		using ConnectionKey = std::tuple<int32_t, int32_t, uint32_t, int32_t, int32_t>;
		// (commodity, other tile x, other tile z); the own tile is fixed per session.
		using DealKey = std::tuple<uint32_t, int32_t, int32_t>;

		std::set<ConnectionKey> mirroredConnections;
		std::set<DealKey> mirroredDeals;
	};
}
