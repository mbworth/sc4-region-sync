/*
 * sc4-region-sync
 *
 * NeighborMirror.h - creates, in this instance, the connections and deals
 * another instance published (docs/PLAN-PHASE2.md), and removes the ones a
 * live publisher stopped listing (docs/PLAN-REMOVAL.md). Game-facing like
 * the bridge; never touches the message system.
 */

#pragma once
#include "NeighborSnapshot.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
		bool allowRecordInjection = false;   // link missing connections into the publisher's record copy; else find and SetIsActive
		uint64_t ownLoadedAt = 0;   // own city load time; a publisher whose city loaded earlier wins on first sighting
		cISC4City* pCity = nullptr;
		bool layEdgeNetwork = false;   // lay the edge stub under each mirrored connection (Q10)
		bool publisherAlive = false;   // WriterLiveness == Alive at mirror time
		bool syncRemovals = false;     // remove what the publisher stopped listing (Q11)
		bool cheat = false;            // RegionSyncMirror: lay again even when tried this session
	};

	struct ConnectionMirrorResult
	{
		int created = 0;    // our own connections created
		int injected = 0;   // connections pushed onto the publisher's record copy
		int laid = 0;       // edge stubs laid
		int removed = 0;    // connections deleted because the publisher stopped listing them
	};

	struct DealMirrorResult
	{
		int created = 0;
		int updated = 0;   // existing deals whose amount followed the publisher
		int cancelled = 0;
	};

	class NeighborMirror
	{
	public:
		ConnectionMirrorResult MirrorConnections(const MirrorContext& ctx, const NeighborSnapshot& publisher);
		DealMirrorResult MirrorDeals(const MirrorContext& ctx, const NeighborSnapshot& publisher);

		// Call when the city closes; the sets are per city session.
		void Reset();
		// Drops a tile's history: its snapshot is Dead or missing from the store.
		void ForgetPublisher(int32_t x, int32_t z);
		// Tiles with history, for the director's missing check.
		std::vector<std::pair<int32_t, int32_t>> Publishers() const;

	private:
		static void SetAmountLogged(cISC4NeighborDeal* deal, int32_t amount);
		// False when the amount could not be followed (locked or dead flags).
		bool FollowAmount(cISC4NeighborDeal* deal, int32_t amount, DealMirrorResult& result);
		// (publisherX, publisherZ, type, innerX, innerZ), publisher's frame.
		using ConnectionKey = std::tuple<int32_t, int32_t, uint32_t, int32_t, int32_t>;
		// (commodity, other tile x, other tile z); the own tile is fixed per session.
		using DealKey = std::tuple<uint32_t, int32_t, int32_t>;

		struct PublisherSession
		{
			std::string host;
			uint32_t pid = 0;
			uint64_t start = 0;
			uint64_t cityLoadedAt = 0;
			bool operator==(const PublisherSession& o) const
			{
				return SameHost(host, o.host) && pid == o.pid && start == o.start && cityLoadedAt == o.cityLoadedAt;
			}
		};
		struct SeenDeal
		{
			int32_t amount = 0;
			uint32_t localType = 0;   // our side's deal type
		};

		// Drops the tile's keys when the publisher session differs from the stored one.
		void CheckPublisherSession(const NeighborSnapshot& a);
		// Removal gates; a closed gate logs once per publisher session and kind.
		bool ListComplete(const NeighborSnapshot& a, bool complete, const char* kind);
		bool RemovalAllowed(const MirrorContext& ctx, const NeighborSnapshot& a, const char* kind, size_t pending);
		void DropHistory(int32_t x, int32_t z);
		void RemoveConnections(const MirrorContext& ctx, const NeighborSnapshot& a, const std::set<ConnectionKey>& listed,
			int32_t ownX, int32_t ownZ, int32_t cells, int32_t aCells, ConnectionMirrorResult& result);
		void CancelDeals(const MirrorContext& ctx, const NeighborSnapshot& a, const std::set<DealKey>& present, DealMirrorResult& result);

		std::map<std::pair<int32_t, int32_t>, PublisherSession> sessions;
		// Value: FindConnection's pointer at the last pass, compared only, never
		// dereferenced. Null with the publisher still listing = tombstone.
		std::map<ConnectionKey, cISC4NeighborConnection*> mirroredConnections;
		// Objects we linked into a record's +0x90 list, compared only.
		struct Injected
		{
			cISC4RegionalCity* record = nullptr;
			cISC4NeighborConnection* connection = nullptr;
		};
		std::map<ConnectionKey, Injected> injectedRecord;
		std::set<std::tuple<int32_t, int32_t, std::string>> gateLogged;   // (tile, kind)
		std::set<ConnectionKey> edgeLayTried;   // timed path lays once per session
		std::set<ConnectionKey> deleteFailed;   // DeleteConnection left it in place last pass
		// Last amount seen from the publisher. Present = seen this session.
		std::map<DealKey, SeenDeal> seenDeals;
	};
}
