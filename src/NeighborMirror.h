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
#include <map>
#include <set>
#include <tuple>

class cIGZCOM;
class cISC4City;
class cISC4NeighborConnection;
class cISC4NeighborDeal;
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
		cIGZCOM* pCOM = nullptr;   // creates connection objects for the record-side step
		bool allowRecordInjection = false;   // RegionSyncMirror only; otherwise find and SetIsActive
		uint64_t ownLoadedAt = 0;   // own city load time; a publisher whose city loaded earlier wins on first sighting
		cISC4City* pCity = nullptr;
		bool layEdgeNetwork = false;   // lay the edge stub under each mirrored connection (Q10)
	};

	struct ConnectionMirrorResult
	{
		int created = 0;    // our own connections created
		int injected = 0;   // connections pushed onto the publisher's record copy
		int laid = 0;       // edge stubs laid
	};

	struct DealMirrorResult
	{
		int created = 0;
		int updated = 0;   // existing deals whose amount followed the publisher
	};

	class NeighborMirror
	{
	public:
		ConnectionMirrorResult MirrorConnections(const MirrorContext& ctx, const NeighborSnapshot& publisher);
		DealMirrorResult MirrorDeals(const MirrorContext& ctx, const NeighborSnapshot& publisher);

		// Call when the city closes; the sets are per city session.
		void Reset();

	private:
		static void SetAmountLogged(cISC4NeighborDeal* deal, int32_t amount);
		// False when the amount could not be followed (locked or dead flags).
		bool FollowAmount(cISC4NeighborDeal* deal, int32_t amount, DealMirrorResult& result);
		// (publisherX, publisherZ, type, innerX, innerZ), publisher's frame.
		using ConnectionKey = std::tuple<int32_t, int32_t, uint32_t, int32_t, int32_t>;
		// (commodity, other tile x, other tile z); the own tile is fixed per session.
		using DealKey = std::tuple<uint32_t, int32_t, int32_t>;

		std::set<ConnectionKey> mirroredConnections;
		std::set<ConnectionKey> edgeLayTried;   // timed path lays once per session
		// Last amount seen from the publisher. Present = seen this session.
		std::map<DealKey, int32_t> seenDeals;
	};
}
