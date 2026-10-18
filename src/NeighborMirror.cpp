/*
 * sc4-region-sync
 *
 * NeighborMirror.cpp
 *
 * Every game call here is new (docs/OPEN-QUESTIONS.md Q9), so each one is
 * logged at Info before and after, with a flush before the call.
 */

#include "NeighborMirror.h"
#include "EdgeNetwork.h"
#include "Game641.h"
#include "NeighborsSim641.h"
#include "RegionalCity641.h"
#include "RegionalCityBridge.h"
#include "cIGZCOM.h"
#include "cIGZDate.h"
#include "cISC4NeighborConnection.h"
#include "cISC4NeighborDeal.h"
#include "cISC4NeighborsSimulator.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cISC4Simulator.h"
#include "cRZAutoRefCount.h"
#include "SC4List.h"
#include "SC4Vector.h"
#include "Logger.h"
#include <algorithm>

namespace
{
	bool FindTileSize(cISC4Region* pRegion, int32_t x, int32_t z, uint32_t& sizeEnum)
	{
		SC4Vector<cISC4Region::cLocation> locations;
		pRegion->GetCityLocations(locations);
		for (size_t i = 0; i < locations.size(); i++)
		{
			if (static_cast<int32_t>(locations[i].x) == x && static_cast<int32_t>(locations[i].z) == z)
			{
				sizeEnum = static_cast<uint32_t>(locations[i].cityTileSize);
				return sizeEnum <= 2;
			}
		}
		return false;
	}

	// Logs the record's connections (up to 16) and checks the raw +0x90 list
	// against GetAllConnections: sane root links, at most 256 nodes, same
	// element pointers in the same order. False means nothing may be pushed.
	bool CheckRecordList(cISC4RegionalCity* pA, int32_t aX, int32_t aZ)
	{
		using Node = SC4ListNode<cISC4NeighborConnection*>;
		constexpr size_t kMaxNodes = 256;
		Logger& logger = Logger::GetInstance();
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: (%d,%d) record %p GetAllConnections ...", aX, aZ, static_cast<void*>(pA));
		logger.Flush();
		SC4List<cISC4NeighborConnection*> copy;
		pA->GetAllConnections(copy);
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: (%d,%d) GetAllConnections count=%zu", aX, aZ, copy.size());
		size_t i = 0;
		for (cISC4NeighborConnection* e : copy)
		{
			if (i++ >= 16)
			{
				break;
			}
			int32_t x = 0, z = 0;
			if (e)
			{
				e->GetConnectionCell(x, z);
			}
			logger.WriteLineFormatted(LogLevel::Info, "RecordConnection:   [%zu] %p type=%u cell=(%d,%d) active=%d",
				i - 1, static_cast<void*>(e), e ? static_cast<uint32_t>(e->GetType()) : 0u, x, z, e && e->GetIsActive() ? 1 : 0);
		}
		logger.Flush();

		const char* failure = nullptr;
		const Node* root = regionsync::rc641::ConnectionListRoot(pA);
		if (!root || !root->next || !root->prev || root->next->prev != root || root->prev->next != root)
		{
			failure = "root links";
		}
		else
		{
			auto it = copy.begin();
			size_t n = 0;
			for (const Node* node = root->next; node != root; node = node->next, ++it, ++n)
			{
				if (!node || n >= kMaxNodes || it == copy.end() || node->value != *it)
				{
					failure = "elements";
					break;
				}
			}
			if (!failure && it != copy.end())
			{
				failure = "length";
			}
		}
		if (failure)
		{
			logger.WriteLineFormatted(LogLevel::Error, "RecordConnection: +0x90 list check failed (%s, root=%p); no injection.", failure, static_cast<const void*>(root));
			return false;
		}
		return true;
	}

	// Creates the connection at c's cell, Repairs it against pA and links it
	// into pA's +0x90 list. Returns it when FindConnection then finds it.
	cISC4NeighborConnection* InjectRecordConnection(const regionsync::MirrorContext& ctx, cISC4RegionalCity* pA, const regionsync::ConnectionRecord& c)
	{
		namespace rc641 = regionsync::rc641;
		Logger& logger = Logger::GetInstance();
		cRZAutoRefCount<cISC4NeighborConnection> pNew;
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: GetClassObject(0x%08X, 0x%08X) ...", rc641::kClsidNeighborConnection, rc641::kIIDNeighborConnection);
		logger.Flush();
		const bool ok = ctx.pCOM->GetClassObject(rc641::kClsidNeighborConnection, rc641::kIIDNeighborConnection, pNew.AsPPVoid());
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: GetClassObject=%d object=%p", ok ? 1 : 0, static_cast<void*>(pNew));
		if (!ok || !pNew)
		{
			return nullptr;
		}
		rc641::InitRawConnection(pNew, c.type, c.cellX, c.cellZ);
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: raw type=%u cell=(%d,%d) written; Repair(%p) ...", c.type, c.cellX, c.cellZ, static_cast<void*>(pA));
		logger.Flush();
		pNew->Repair(pA);
		cISC4RegionalCity* pOther = pNew->GetCityOnOtherSideOfConnection();
		int32_t ox = 0, oz = 0;
		if (pOther)
		{
			pOther->GetPosition(ox, oz);
		}
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: Repair returned, other side=%p tile=(%d,%d), own=%p, active=%d",
			static_cast<void*>(pOther), ox, oz, static_cast<void*>(ctx.pOwnRegionalCity), pNew->GetIsActive() ? 1 : 0);
		if (pOther != ctx.pOwnRegionalCity)
		{
			logger.WriteLine(LogLevel::Info, "RecordConnection: other side is not our city, skipped.");
			return nullptr;
		}

		logger.WriteLine(LogLevel::Info, "RecordConnection: insert at the end of the record's +0x90 list ...");
		logger.Flush();
		SC4List<cISC4NeighborConnection*>& list = rc641::ConnectionList(pA);
		cISC4NeighborConnection* raw = pNew;
		const auto node = list.insert(list.end(), raw);
		raw->AddRef();   // the list's reference, as the game's own inserts take
		cISC4NeighborConnection* pCheck = rc641::FindConnection(pA, c.type, c.cellX, c.cellZ);
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: FindConnection=%p (expected %p)", static_cast<void*>(pCheck), static_cast<void*>(raw));
		if (pCheck != raw)
		{
			list.erase(node);   // Releases the list's reference
			logger.WriteLine(LogLevel::Error, "RecordConnection: FindConnection does not see the inserted object; removed again.");
			return nullptr;
		}
		return raw;
	}

	// The record-side step (record-connections-641.md): make sure the
	// publisher's record copy in this process holds the connection at its
	// cell, active, so FindConnectionOnOtherSide and the buy-side deal check
	// find it. Injection only when ctx.allowRecordInjection. Returns the
	// object linked in, or null. listState: 0 = not checked this call,
	// 1 = ok, 2 = check failed.
	cISC4NeighborConnection* EnsureRecordConnection(const regionsync::MirrorContext& ctx, int32_t aX, int32_t aZ, int32_t aCells,
		const regionsync::ConnectionRecord& c, cISC4NeighborConnection* bConn, int& listState)
	{
		namespace rc641 = regionsync::rc641;
		Logger& logger = Logger::GetInstance();
		if (!c.cellKnown || !regionsync::IsConnectionCellAdjacent(c.innerX, c.innerZ, c.cellX, c.cellZ, aCells))
		{
			logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: type %u inner=(%d,%d) cell=(%d,%d) known=%d cellsA=%d not usable, skipped.",
				c.type, c.innerX, c.innerZ, c.cellX, c.cellZ, c.cellKnown ? 1 : 0, aCells);
			return nullptr;
		}
		// ReloadCity replaces record objects: resolve every time.
		cISC4RegionalCity* pA = regionsync::FindRegionalCity(ctx.pRegion, aX, aZ);
		if (!pA)
		{
			logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: no record for (%d,%d), skipped.", aX, aZ);
			return nullptr;
		}
		if (ctx.allowRecordInjection && listState == 0)
		{
			listState = CheckRecordList(pA, aX, aZ) ? 1 : 2;
		}

		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: FindConnection(%u, %d, %d) on record %p ...", c.type, c.cellX, c.cellZ, static_cast<void*>(pA));
		logger.Flush();
		cISC4NeighborConnection* pRec = rc641::FindConnection(pA, c.type, c.cellX, c.cellZ);
		logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: FindConnection=%p active=%d", static_cast<void*>(pRec), pRec && pRec->GetIsActive() ? 1 : 0);
		logger.Flush();

		cISC4NeighborConnection* injected = nullptr;
		if (pRec)
		{
			if (!pRec->GetIsActive())
			{
				logger.WriteLine(LogLevel::Info, "RecordConnection: SetIsActive(true) ...");
				logger.Flush();
				pRec->SetIsActive(true);
				logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: SetIsActive returned, active=%d", pRec->GetIsActive() ? 1 : 0);
			}
		}
		else
		{
			if (!ctx.allowRecordInjection || listState != 1 || !ctx.pCOM)
			{
				logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: not injected (allowed=%d, list check=%d, COM=%p).",
					ctx.allowRecordInjection ? 1 : 0, listState, static_cast<void*>(ctx.pCOM));
				return nullptr;
			}
			injected = InjectRecordConnection(ctx, pA, c);
			if (!injected)
			{
				return nullptr;
			}
		}

		if (ctx.pNeighborsSim && bConn)
		{
			logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: FindConnectionOnOtherSide(%p) ...", static_cast<void*>(bConn));
			logger.Flush();
			cISC4NeighborConnection* pOtherSide = ctx.pNeighborsSim->FindConnectionOnOtherSide(bConn);
			logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: FindConnectionOnOtherSide=%p active=%d",
				static_cast<void*>(pOtherSide), pOtherSide && pOtherSide->GetIsActive() ? 1 : 0);
		}
		logger.Flush();
		return injected;
	}

	// Per entry of the publisher's list (docs/PLAN-DUAL-FERRY.md rules 5 to 7).
	enum class LaneRole { Single, Unpaired, Lo, Hi, Duplicate };
	struct Lane
	{
		LaneRole role = LaneRole::Single;
		bool dir = false;        // CreateConnection's marker direction
		size_t partner = 0;      // Hi: its lo
		bool onEdge = false;     // passed the create pass's checks
		int32_t bx = 0, bz = 0;  // our frame
	};

	// gateLogged kind for a once-per-lane log line.
	std::string LaneGate(const char* kind, uint32_t type, int32_t innerX, int32_t innerZ)
	{
		return std::string(kind) + " " + std::to_string(type) + " " + std::to_string(innerX) + " " + std::to_string(innerZ);
	}

	size_t OwnConnectionCount(cISC4NeighborsSimulator* pSim)
	{
		SC4List<cISC4NeighborConnection*> list;
		pSim->GetAllConnections(list);
		return list.size();
	}
}

namespace regionsync
{
	void NeighborMirror::Reset()
	{
		sessions.clear();
		mirroredConnections.clear();
		injectedRecord.clear();
		gateLogged.clear();
		deleteFailed.clear();
		edgeLayTried.clear();
		seenDeals.clear();
	}

	void NeighborMirror::DropHistory(int32_t x, int32_t z)
	{
		const auto ofTile = [x, z](const auto& key) { return std::get<0>(key) == x && std::get<1>(key) == z; };
		std::erase_if(mirroredConnections, [&](const auto& e) { return ofTile(e.first); });
		std::erase_if(injectedRecord, [&](const auto& e) { return ofTile(e.first); });
		std::erase_if(edgeLayTried, ofTile);
		std::erase_if(gateLogged, ofTile);
		std::erase_if(deleteFailed, ofTile);
		std::erase_if(seenDeals, [&](const auto& e) { return std::get<1>(e.first) == x && std::get<2>(e.first) == z; });
	}

	void NeighborMirror::ForgetPublisher(int32_t x, int32_t z)
	{
		if (sessions.erase({ x, z }))
		{
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Mirror (%d,%d): snapshot dead or missing, history dropped", x, z);
		}
		DropHistory(x, z);
	}

	std::vector<std::pair<int32_t, int32_t>> NeighborMirror::Publishers() const
	{
		std::vector<std::pair<int32_t, int32_t>> tiles;
		for (const auto& [tile, session] : sessions) tiles.push_back(tile);
		return tiles;
	}

	// A relaunched or reloaded publisher converges by creation only.
	void NeighborMirror::CheckPublisherSession(const NeighborSnapshot& a)
	{
		const PublisherSession now{ a.writer.host, a.writer.pid, a.writer.start, a.cityLoadedAt };
		const auto it = sessions.find({ a.regionX, a.regionZ });
		if (it != sessions.end() && it->second == now)
		{
			return;
		}
		Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Mirror (%d,%d): publisher session changed, history dropped (host=%s pid=%u start=%llu loadedAt=%llu, %s)",
			a.regionX, a.regionZ, now.host.c_str(), now.pid, static_cast<unsigned long long>(now.start), static_cast<unsigned long long>(now.cityLoadedAt),
			it == sessions.end() ? "none stored" : "differs");
		DropHistory(a.regionX, a.regionZ);
		sessions[{ a.regionX, a.regionZ }] = now;
	}

	ConnectionMirrorResult NeighborMirror::MirrorConnections(const MirrorContext& ctx, const NeighborSnapshot& a)
	{
		Logger& logger = Logger::GetInstance();
		ConnectionMirrorResult result;
		CheckPublisherSession(a);
		if (!ctx.pRegion || !ctx.pOwnRegionalCity || !ctx.pNeighborsSim)
		{
			logger.WriteLine(LogLevel::Error, "MirrorConnections: no region, city or neighbors simulator.");
			return result;
		}
		if (!a.connectionsKnown)
		{
			logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: (%d,%d) snapshot has no connection list (schema 1?), skipped.", a.regionX, a.regionZ);
			return result;
		}

		int32_t ownX = 0, ownZ = 0, sizeX = 0, sizeZ = 0;
		ctx.pOwnRegionalCity->GetPosition(ownX, ownZ);
		ctx.pOwnRegionalCity->GetCitySize(sizeX, sizeZ);
		uint32_t sizeEnum = 0;
		if (!FindTileSize(ctx.pRegion, ownX, ownZ, sizeEnum))
		{
			logger.WriteLineFormatted(LogLevel::Error, "MirrorConnections: own tile (%d,%d) not in the region's location list, skipped.", ownX, ownZ);
			return result;
		}
		const int32_t cells = CityCells(sizeEnum);
		// The snapshot carries GetCitySize, not the size enum; the location list does.
		uint32_t aSizeEnum = 0;
		const int32_t aCells = FindTileSize(ctx.pRegion, a.regionX, a.regionZ, aSizeEnum) ? CityCells(aSizeEnum) : 0;
		logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: from (%d,%d) '%s', %zu connection(s), cellsA=%d (0: dual lanes cannot pair). Own tile (%d,%d) GetCitySize=(%d,%d) sizeEnum=%u cells=%d",
			a.regionX, a.regionZ, a.cityName.c_str(), a.connections.size(), aCells, ownX, ownZ, sizeX, sizeZ, sizeEnum, cells);

		// Pair pass: pairs in the publisher's frame, kept when both lanes convert to one of our edges.
		const auto keyOf = [&a](const ConnectionRecord& c) { return ConnectionKey{ a.regionX, a.regionZ, c.type, c.innerX, c.innerZ }; };
		const auto onOurEdge = [&](const ConnectionRecord& c, int32_t& bx, int32_t& bz, uint32_t& side) {
			if (c.otherTileX != ownX || c.otherTileZ != ownZ) return false;
			if (!ToSubscriberFrame(a.regionX, a.regionZ, c.innerX, c.innerZ, ownX, ownZ, cells, bx, bz)) return false;
			const std::optional<EdgeStub> stub = EdgeStubGeometry(bx, bz, cells);
			side = stub ? stub->side : 0;
			return stub.has_value();
		};
		std::vector<Lane> lanes(a.connections.size());
		const bool anyDual = std::any_of(a.connections.begin(), a.connections.end(), [](const ConnectionRecord& c) { return IsDualConnectionType(c.type); });
		const bool driveOnRight = anyDual ? game641::DriveOnRight() : true;
		for (size_t i = 0; i < lanes.size(); i++)
		{
			int32_t bx = 0, bz = 0;
			uint32_t side = 0;
			if (!IsDualConnectionType(a.connections[i].type)) continue;
			lanes[i].role = LaneRole::Unpaired;
			// Vanilla gives an unpaired lane primary 0.
			if (onOurEdge(a.connections[i], bx, bz, side)) lanes[i].dir = DualLaneDir(side, false, driveOnRight);
		}
		for (const auto& [loIndex, hiIndex] : PairDualLanes(a.connections, aCells))
		{
			const ConnectionRecord& lo = a.connections[loIndex];
			const ConnectionRecord& hi = a.connections[hiIndex];
			int32_t lx = 0, lz = 0, hx = 0, hz = 0;
			uint32_t loSide = 0, hiSide = 0;
			// Same publisher edge implies the same side here; the side test is defensive.
			if (!onOurEdge(lo, lx, lz, loSide) || !onOurEdge(hi, hx, hz, hiSide) || loSide != hiSide) continue;
			lanes[loIndex] = { LaneRole::Lo, DualLaneDir(loSide, false, driveOnRight) };
			lanes[hiIndex] = { LaneRole::Hi, DualLaneDir(hiSide, true, driveOnRight), loIndex };
			if (ctx.cheat || !mirroredConnections.count(keyOf(lo)) || !mirroredConnections.count(keyOf(hi)))
			{
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: pair type %u lo inner=(%d,%d) -> (%d,%d) dir=%d, hi inner=(%d,%d) -> (%d,%d) dir=%d, side %u driveOnRight=%d",
					lo.type, lo.innerX, lo.innerZ, lx, lz, lanes[loIndex].dir ? 1 : 0, hi.innerX, hi.innerZ, hx, hz, lanes[hiIndex].dir ? 1 : 0, hiSide, driveOnRight ? 1 : 0);
			}
		}
		// A repeated entry of a paired lane follows that lane and is never laid on its own.
		for (size_t i = 0; i < lanes.size(); i++)
		{
			if (lanes[i].role != LaneRole::Unpaired) continue;
			for (size_t j = 0; j < lanes.size(); j++)
			{
				if ((lanes[j].role == LaneRole::Lo || lanes[j].role == LaneRole::Hi) && keyOf(a.connections[j]) == keyOf(a.connections[i]))
				{
					lanes[i] = { LaneRole::Duplicate };
					break;
				}
			}
		}

		int listState = 0;
		std::set<ConnectionKey> listed;
		for (size_t index = 0; index < a.connections.size(); index++)
		{
			const ConnectionRecord& c = a.connections[index];
			Lane& lane = lanes[index];
			if (c.otherTileX != ownX || c.otherTileZ != ownZ)
			{
				continue;
			}
			listed.insert(keyOf(c));
			if (c.type == static_cast<uint32_t>(cISC4NeighborsSimulator::ConnectionType::Ferry))
			{
				// Not mirrored (rule 12). First-time game call: cheat path only.
				int32_t fx = 0, fz = 0;
				if (!ctx.cheat) continue;
				if (!ToSubscriberFrame(a.regionX, a.regionZ, c.innerX, c.innerZ, ownX, ownZ, cells, fx, fz))
				{
					logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: ferry inner=(%d,%d) converts to (%d,%d), not on our edge, not mirrored.", c.innerX, c.innerZ, fx, fz);
					continue;
				}
				if (!gateLogged.insert({ a.regionX, a.regionZ, "ferry" }).second) continue;
				cISC4RegionalCity* pA = FindRegionalCity(ctx.pRegion, a.regionX, a.regionZ);
				if (!pA)
				{
					logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: ferry inner=(%d,%d) -> our cell (%d,%d), not mirrored; publisher record not found, not queried; logged once per publisher session",
						c.innerX, c.innerZ, fx, fz);
					continue;
				}
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: ferry inner=(%d,%d) -> our cell (%d,%d), not mirrored. GetConnectionsOfTypeToCity(9, %p) ...",
					c.innerX, c.innerZ, fx, fz, static_cast<void*>(pA));
				logger.Flush();
				SC4List<cISC4NeighborConnection*> ferries;
				ctx.pNeighborsSim->GetConnectionsOfTypeToCity(cISC4NeighborsSimulator::ConnectionType::Ferry, pA, ferries);
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: GetConnectionsOfTypeToCity count=%zu: %s ferry to (%d,%d) here (any, coarse); logged once per publisher session",
					ferries.size(), ferries.empty() ? "no" : "a", a.regionX, a.regionZ);
				continue;
			}
			if (!IsMirrorableConnectionType(c.type))
			{
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: type %u not in the slice 1 whitelist, skipped.", c.type);
				continue;
			}
			int32_t bx = 0, bz = 0;
			if (!ToSubscriberFrame(a.regionX, a.regionZ, c.innerX, c.innerZ, ownX, ownZ, cells, bx, bz))
			{
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: type %u inner=(%d,%d) converts to (%d,%d), not on our edge, skipped.",
					c.type, c.innerX, c.innerZ, bx, bz);
				continue;
			}
			lane.onEdge = true;
			lane.bx = bx;
			lane.bz = bz;
			const ConnectionKey key = keyOf(c);
			const auto known = mirroredConnections.find(key);
			if (known != mirroredConnections.end())
			{
				deleteFailed.erase(key);
				// Pointer for this pass. Null while still listed is a tombstone: not re-created.
				const auto type = static_cast<cISC4NeighborsSimulator::ConnectionType>(c.type);
				cISC4NeighborConnection* pNow = ctx.pNeighborsSim->FindConnection(type, bx, bz);
				if (pNow != known->second)
				{
					logger.WriteLineFormatted(LogLevel::Info, pNow ? "MirrorConnections: type %u (%d,%d) connection %p -> %p, refreshed"
						: "MirrorConnections: type %u (%d,%d) connection %p -> %p, tombstone (gone here, publisher still lists it)",
						c.type, bx, bz, static_cast<void*>(known->second), static_cast<void*>(pNow));
					known->second = pNow;
				}
			}
			else
			{
				const auto type = static_cast<cISC4NeighborsSimulator::ConnectionType>(c.type);
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: type %u inner=(%d,%d) -> our cell (%d,%d). HasConnection ...",
					c.type, c.innerX, c.innerZ, bx, bz);
				logger.Flush();
				const bool has = ctx.pNeighborsSim->HasConnection(type, bx, bz);
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: HasConnection=%d", has ? 1 : 0);
				logger.Flush();
				if (!has)
				{
					logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: CreateConnection(%u, %d, %d, %d) ...", c.type, bx, bz, lane.dir ? 1 : 0);
					logger.Flush();
					// The return value is not set by the game (connections-641.md); FindConnection confirms.
					ctx.pNeighborsSim->CreateConnection(type, bx, bz, lane.dir);
					logger.WriteLine(LogLevel::Info, "MirrorConnections: CreateConnection returned");
					logger.Flush();
				}
				cISC4NeighborConnection* pFound = ctx.pNeighborsSim->FindConnection(type, bx, bz);
				logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: FindConnection=%p", static_cast<void*>(pFound));
				logger.Flush();
				if (pFound && !has)
				{
					result.created++;
				}
				// A's record copy here may lack the connection even when we have ours.
				// Our own count around it shows a feedback through the 0xCA63D517 path.
				if (pFound)
				{
					logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: own GetAllConnections count before=%zu", OwnConnectionCount(ctx.pNeighborsSim));
					if (cISC4NeighborConnection* pInjected = EnsureRecordConnection(ctx, a.regionX, a.regionZ, aCells, c, pFound, listState))
					{
						injectedRecord[key] = { FindRegionalCity(ctx.pRegion, a.regionX, a.regionZ), pInjected };
						result.injected++;
					}
					logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: own GetAllConnections count after=%zu", OwnConnectionCount(ctx.pNeighborsSim));
				}
				mirroredConnections[key] = pFound;
			}
		}

		// Lay pass. Idempotent, so it runs for every connection, not only new
		// ones; once per key per session on the timed path, since a failed lay
		// has already demolished the cells.
		for (size_t index = 0; ctx.layEdgeNetwork && index < a.connections.size(); index++)
		{
			const ConnectionRecord& c = a.connections[index];
			const Lane& lane = lanes[index];
			if (!lane.onEdge || lane.role == LaneRole::Lo || lane.role == LaneRole::Duplicate) continue;
			const ConnectionKey key = keyOf(c);
			const auto type = static_cast<cISC4NeighborsSimulator::ConnectionType>(c.type);
			if (lane.role == LaneRole::Unpaired)
			{
				if (ctx.cheat || gateLogged.insert({ a.regionX, a.regionZ, LaneGate("unpaired", c.type, c.innerX, c.innerZ) }).second)
				{
					logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: type %u inner=(%d,%d) -> (%d,%d) is an unpaired dual lane, connection only; logged once per key",
						c.type, c.innerX, c.innerZ, lane.bx, lane.bz);
				}
				continue;
			}
			if (lane.role == LaneRole::Single)
			{
				if (!ctx.cheat && edgeLayTried.count(key)) continue;
				edgeLayTried.insert(key);
				cISC4NeighborConnection* pConn = ctx.pNeighborsSim->FindConnection(type, lane.bx, lane.bz);
				logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: FindConnection(%u, %d, %d)=%p", c.type, lane.bx, lane.bz, static_cast<void*>(pConn));
				if (pConn)
				{
					const EdgeLayResult lay = LayEdgePiece(ctx, c.type, lane.bx, lane.bz, cells);
					logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: type %u (%d,%d) laid=%d (%s)", c.type, lane.bx, lane.bz, lay.laid ? 1 : 0, lay.reason);
					if (lay.laid) result.laid++;
				}
				continue;
			}
			// Hi: lays the pair once both lanes exist here; only then are both keys tried.
			const Lane& lo = lanes[lane.partner];
			const ConnectionKey loKey = keyOf(a.connections[lane.partner]);
			if (!lo.onEdge || (!ctx.cheat && edgeLayTried.count(loKey) && edgeLayTried.count(key))) continue;
			cISC4NeighborConnection* pLo = ctx.pNeighborsSim->FindConnection(type, lo.bx, lo.bz);
			cISC4NeighborConnection* pHi = ctx.pNeighborsSim->FindConnection(type, lane.bx, lane.bz);
			if (!pLo || !pHi)
			{
				if (ctx.cheat || gateLogged.insert({ a.regionX, a.regionZ, LaneGate("pair missing", c.type, c.innerX, c.innerZ) }).second)
				{
					logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair type %u FindConnection lo (%d,%d)=%p hi (%d,%d)=%p, a lane is missing here, not laid",
						c.type, lo.bx, lo.bz, static_cast<void*>(pLo), lane.bx, lane.bz, static_cast<void*>(pHi));
				}
				continue;
			}
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair type %u FindConnection lo (%d,%d)=%p hi (%d,%d)=%p",
				c.type, lo.bx, lo.bz, static_cast<void*>(pLo), lane.bx, lane.bz, static_cast<void*>(pHi));
			const EdgeLayResult lay = LayEdgePair(ctx, c.type, lo.bx, lo.bz, lane.bx, lane.bz, cells);
			edgeLayTried.insert(loKey);
			edgeLayTried.insert(key);
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair type %u lo (%d,%d) hi (%d,%d) laid=%d (%s)", c.type, lo.bx, lo.bz, lane.bx, lane.bz, lay.laid ? 1 : 0, lay.reason);
			if (lay.laid) result.laid++;
		}
		RemoveConnections(ctx, a, listed, ownX, ownZ, cells, aCells, result);
		return result;
	}

	// Only a complete list says anything about unlisted keys: an incomplete one
	// keeps them all, tombstones included.
	bool NeighborMirror::ListComplete(const NeighborSnapshot& a, bool complete, const char* kind)
	{
		if (complete)
		{
			return true;
		}
		if (gateLogged.insert({ a.regionX, a.regionZ, std::string(kind) + " incomplete" }).second)
		{
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Removal: (%d,%d) %s list incomplete, unlisted keys kept; logged once per publisher session",
				a.regionX, a.regionZ, kind);
		}
		return false;
	}

	bool NeighborMirror::RemovalAllowed(const MirrorContext& ctx, const NeighborSnapshot& a, const char* kind, size_t pending)
	{
		if (ctx.publisherAlive && a.cityLoadedAt != 0 && ctx.syncRemovals)
		{
			return true;
		}
		if (gateLogged.insert({ a.regionX, a.regionZ, kind }).second)
		{
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Removal: (%d,%d) %zu unlisted %s(s), not removed, keys dropped (alive=%d loadedAt=%d syncRemovals=%d); logged once per publisher session",
				a.regionX, a.regionZ, pending, kind, ctx.publisherAlive ? 1 : 0, a.cityLoadedAt != 0 ? 1 : 0, ctx.syncRemovals ? 1 : 0);
		}
		return false;
	}

	// Keys of this publisher it no longer lists (docs/PLAN-REMOVAL.md).
	void NeighborMirror::RemoveConnections(const MirrorContext& ctx, const NeighborSnapshot& a, const std::set<ConnectionKey>& listed,
		int32_t ownX, int32_t ownZ, int32_t cells, int32_t aCells, ConnectionMirrorResult& result)
	{
		Logger& logger = Logger::GetInstance();
		const auto forget = [&](const ConnectionKey& key) {
			const auto& [ax, az, connType, innerX, innerZ] = key;
			mirroredConnections.erase(key);
			injectedRecord.erase(key);
			edgeLayTried.erase(key);
			deleteFailed.erase(key);
			gateLogged.erase({ ax, az, LaneGate("unpaired", connType, innerX, innerZ) });
			gateLogged.erase({ ax, az, LaneGate("pair missing", connType, innerX, innerZ) });
		};
		std::vector<ConnectionKey> unlisted;
		for (const auto& [key, pointer] : mirroredConnections)
		{
			if (std::get<0>(key) == a.regionX && std::get<1>(key) == a.regionZ && !listed.count(key)) unlisted.push_back(key);
		}
		if (unlisted.empty())
		{
			return;
		}
		// The publisher stopped listing them: our memory goes either way.
		if (!ListComplete(a, a.connectionsComplete, "connection"))
		{
			return;
		}
		if (!RemovalAllowed(ctx, a, "connection", unlisted.size()))
		{
			for (const ConnectionKey& key : unlisted) forget(key);
			return;
		}

		int listState = 0;
		for (const ConnectionKey& key : unlisted)
		{
			const auto& [ax, az, connType, innerX, innerZ] = key;
			const auto type = static_cast<cISC4NeighborsSimulator::ConnectionType>(connType);
			const auto dropKey = [&] { forget(key); };
			int32_t bx = 0, bz = 0;
			if (!ToSubscriberFrame(ax, az, innerX, innerZ, ownX, ownZ, cells, bx, bz))
			{
				logger.WriteLineFormatted(LogLevel::Info, "Removal: connection type %u inner=(%d,%d) not on our edge, key dropped.", connType, innerX, innerZ);
				dropKey();
				continue;
			}
			cISC4NeighborConnection* pConn = ctx.pNeighborsSim->FindConnection(type, bx, bz);
			cISC4NeighborConnection* stored = mirroredConnections[key];
			if (!pConn)
			{
				logger.WriteLineFormatted(LogLevel::Info, "Removal: connection type %u (%d,%d) already gone here, key dropped.", connType, bx, bz);
				dropKey();
				continue;
			}
			if (pConn != stored)
			{
				logger.WriteLineFormatted(LogLevel::Info, "Removal: connection type %u (%d,%d) %p -> %p, rebuilt since last pass, kept as the player's own, key dropped.",
					connType, bx, bz, static_cast<void*>(stored), static_cast<void*>(pConn));
				dropKey();
				continue;
			}

			logger.WriteLineFormatted(LogLevel::Info, "Removal: connection type %u inner=(%d,%d) -> (%d,%d) no longer listed by (%d,%d). DeleteConnection ...",
				connType, innerX, innerZ, bx, bz, ax, az);
			logger.Flush();
			ctx.pNeighborsSim->DeleteConnection(type, bx, bz);
			cISC4NeighborConnection* pAfter = ctx.pNeighborsSim->FindConnection(type, bx, bz);
			logger.WriteLineFormatted(LogLevel::Info, "Removal: DeleteConnection returned, FindConnection=%p", static_cast<void*>(pAfter));
			logger.Flush();
			if (pAfter)
			{
				if (deleteFailed.erase(key))
				{
					logger.WriteLineFormatted(LogLevel::Info, "Removal: connection type %u (%d,%d) still present after a second DeleteConnection, key dropped.", connType, bx, bz);
					dropKey();
					continue;
				}
				logger.WriteLineFormatted(LogLevel::Info, "Removal: connection type %u (%d,%d) still present, no edge clear or record step; retried next pass.", connType, bx, bz);
				mirroredConnections[key] = pAfter;
				deleteFailed.insert(key);
				continue;
			}
			if (ConnectionToNetworkType(connType) >= 0)
			{
				const bool placed = ClearEdgePiece(ctx, connType, bx, bz, cells);
				logger.WriteLineFormatted(LogLevel::Info, "Removal: edge clear type %u (%d,%d) re-solve=%d", connType, bx, bz, placed ? 1 : 0);
			}

			// The publisher's record copy here, or a later save writes a stale active entry into its file.
			int32_t cx = 0, cz = 0;
			cISC4RegionalCity* pA = FindRegionalCity(ctx.pRegion, ax, az);
			const auto injected = injectedRecord.find(key);
			if (!pA || !PublisherConnectionCell(innerX, innerZ, bx, bz, cells, cx, cz) || !IsConnectionCellAdjacent(innerX, innerZ, cx, cz, aCells))
			{
				logger.WriteLineFormatted(LogLevel::Info, "Removal: record copy for (%d,%d) not usable (record=%p cell=(%d,%d) cellsA=%d).",
					ax, az, static_cast<void*>(pA), cx, cz, aCells);
			}
			else
			{
				logger.WriteLineFormatted(LogLevel::Info, "Removal: record FindConnection(%u, %d, %d) on %p ...", connType, cx, cz, static_cast<void*>(pA));
				logger.Flush();
				cISC4NeighborConnection* pRec = rc641::FindConnection(pA, connType, cx, cz);
				const bool ours = pRec && injected != injectedRecord.end() && injected->second.record == pA && injected->second.connection == pRec;
				logger.WriteLineFormatted(LogLevel::Info, "Removal: record FindConnection=%p active=%d injected=%d",
					static_cast<void*>(pRec), pRec && pRec->GetIsActive() ? 1 : 0, ours ? 1 : 0);
				if (pRec)
				{
					logger.Flush();
					pRec->SetIsActive(false);
					logger.WriteLineFormatted(LogLevel::Info, "Removal: record SetIsActive(false) returned, active=%d", pRec->GetIsActive() ? 1 : 0);
				}
				if (ours && listState == 0)
				{
					listState = CheckRecordList(pA, ax, az) ? 1 : 2;
				}
				if (ours && listState == 1)
				{
					SC4List<cISC4NeighborConnection*>& list = rc641::ConnectionList(pA);
					for (auto it = list.begin(); it != list.end(); ++it)
					{
						if (*it != pRec) continue;
						logger.WriteLine(LogLevel::Info, "Removal: erase our object from the record's +0x90 list (Releases the list's reference) ...");
						logger.Flush();
						list.erase(it);
						logger.WriteLineFormatted(LogLevel::Info, "Removal: erased, record FindConnection=%p", static_cast<void*>(rc641::FindConnection(pA, connType, cx, cz)));
						break;
					}
				}
				else if (ours)
				{
					logger.WriteLine(LogLevel::Info, "Removal: record list check failed, our object left inactive in the list.");
				}
			}
			dropKey();
			result.removed++;
		}
	}

	void NeighborMirror::SetAmountLogged(cISC4NeighborDeal* deal, int32_t amount)
	{
		Logger& logger = Logger::GetInstance();
		logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: SetAmount(%d) ...", amount);
		logger.Flush();
		deal->SetAmount(amount);
		logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: SetAmount returned, amount=%d cost=%lld flags=0x%X",
			deal->GetAmount(), static_cast<long long>(deal->GetCost()), static_cast<uint32_t>(deal->GetFlags()));
		logger.Flush();
	}

	// Existing deal whose published amount changed since we last saw it.
	// Only a change on the publisher's side is followed, so two instances
	// following each other converge instead of trading amounts each month.
	bool NeighborMirror::FollowAmount(cISC4NeighborDeal* deal, int32_t amount, DealMirrorResult& result)
	{
		Logger& logger = Logger::GetInstance();
		const int32_t have = deal->GetAmount();
		const uint32_t flags = static_cast<uint32_t>(deal->GetFlags());
		if (have == amount) return true;
		if (flags & (kDealFlagsAmountLocked | kDealFlagsDead))
		{
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: amount %d -> %d not followed, flags=0x%X", have, amount, flags);
			return false;
		}
		logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: follow amount %d -> %d", have, amount);
		SetAmountLogged(deal, amount);
		result.updated++;
		return true;
	}

	DealMirrorResult NeighborMirror::MirrorDeals(const MirrorContext& ctx, const NeighborSnapshot& a)
	{
		Logger& logger = Logger::GetInstance();
		DealMirrorResult result;
		CheckPublisherSession(a);
		if (!ctx.pRegion || !ctx.pOwnRegionalCity || !ctx.pNeighborsSim || !ctx.pSimulator)
		{
			logger.WriteLine(LogLevel::Error, "MirrorDeals: no region, city, neighbors simulator or simulator.");
			return result;
		}
		if (!a.dealsKnown)
		{
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: (%d,%d) snapshot has no deal list (schema 1?), skipped.", a.regionX, a.regionZ);
			return result;
		}

		int32_t ownX = 0, ownZ = 0;
		ctx.pOwnRegionalCity->GetPosition(ownX, ownZ);
		logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: from (%d,%d) '%s', %zu deal(s).", a.regionX, a.regionZ, a.cityName.c_str(), a.deals.size());

		std::set<DealKey> listed;
		std::set<DealKey> present;   // listed and not cancelled, whatever else validation says
		for (const DealRecord& d : a.deals)
		{
			if (d.otherTileX != ownX || d.otherTileZ != ownZ)
			{
				continue;
			}
			const uint32_t flags = MaskDealFlags(d.flags);
			if (IsDealType(d.type) && !(flags & kDealFlagCancelled))
			{
				present.insert({ DealCommodity(d.type), a.regionX, a.regionZ });
			}
			if (IsProposedOnlyDeal(flags))
			{
				logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: type=0x%08X flags=0x%X proposed only, ignored.", d.type, flags);
				if (IsDealType(d.type)) listed.insert({ DealCommodity(d.type), a.regionX, a.regionZ });
				continue;
			}
			if (!IsDealType(d.type) || !IsValidDealAmount(d.amount) || !IsValidExpiresIn(d.expiresIn) || (flags & kDealFlagCancelled))
			{
				logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: type=0x%08X amount=%d expiresIn=%d flags=0x%X rejected by validation, skipped.",
					d.type, d.amount, d.expiresIn, flags);
				continue;
			}

			const uint32_t typeB = OtherSideDealType(d.type);
			const DealKey key{ DealCommodity(d.type), a.regionX, a.regionZ };
			listed.insert(key);
			const auto seen = seenDeals.find(key);
			const bool seenBefore = seen != seenDeals.end();
			const bool changed = !seenBefore || seen->second.amount != d.amount;

			// ReloadCity replaces record objects: resolve every time.
			cISC4RegionalCity* pA = FindRegionalCity(ctx.pRegion, a.regionX, a.regionZ);
			if (!pA)
			{
				logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: no record for (%d,%d), skipped.", a.regionX, a.regionZ);
				continue;
			}

			{
				// Scoped so the list is gone before anything below changes the deal list.
				SC4List<cISC4NeighborDeal*> existing;
				logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: FindDeals(0x%08X, %p) ...", typeB, static_cast<void*>(pA));
				logger.Flush();
				ns641::FindDeals(ctx.pNeighborsSim, static_cast<int32_t>(typeB), pA, existing);
				const size_t n = existing.size();
				logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: FindDeals count=%zu", n);
				if (n > 0)
				{
					// First sighting only records the amount: an existing deal may
					// be the player's own, or a reload's copy (Q9 step 4).
					if (!seenBefore)
					{
						// The city loaded first holds the newer state (vanilla: the
						// neighbor's saved deal wins at load).
						const bool publisherOlder = a.cityLoadedAt != 0 && ctx.ownLoadedAt != 0 && a.cityLoadedAt < ctx.ownLoadedAt;
						if (publisherOlder && n == 1)
						{
							logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: existing deal, publisher loaded earlier, following amount=%d.", d.amount);
							if (FollowAmount(*existing.begin(), d.amount, result))
							{
								seenDeals[key] = { d.amount, typeB };
							}
						}
						else
						{
							logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: existing deal seen (%zu match), amount=%d recorded.", n, d.amount);
							seenDeals[key] = { d.amount, typeB };
						}
					}
					else if (changed && n == 1)
					{
						// Not recorded when the follow was refused, so it is retried.
						if (FollowAmount(*existing.begin(), d.amount, result))
						{
							seenDeals[key] = { d.amount, typeB };
						}
					}
					else if (changed)
					{
						logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: %zu deals match, amount not followed.", n);
					}
					continue;
				}
			}
			if (seenBefore)
			{
				// Present earlier this session and gone now: the player removed it here.
				logger.WriteLine(LogLevel::Info, "MirrorDeals: seen before and no longer present, not re-created.");
				continue;
			}

			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: FindConnectionSuitableForDeal(0x%08X, %p) ...", typeB, static_cast<void*>(pA));
			logger.Flush();
			cISC4NeighborConnection* pSuitable = ctx.pNeighborsSim->FindConnectionSuitableForDeal(static_cast<int32_t>(typeB), pA);
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: FindConnectionSuitableForDeal=%p", static_cast<void*>(pSuitable));

			cRZAutoRefCount<cISC4NeighborDeal> deal;
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: ProposeDeal(0x%08X, %p) ...", typeB, static_cast<void*>(pA));
			logger.Flush();
			const bool proposed = ctx.pNeighborsSim->ProposeDeal(static_cast<int32_t>(typeB), pA, deal.AsPPObj());
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: ProposeDeal=%d deal=%p flags=0x%X",
				proposed ? 1 : 0, static_cast<void*>(static_cast<cISC4NeighborDeal*>(deal)), deal ? static_cast<uint32_t>(deal->GetFlags()) : 0u);
			logger.Flush();
			if (!proposed || !deal)
			{
				continue;
			}

			// Dates while still silent: current = start = today, expiration = today + expiresIn.
			cIGZDate* pSimDate = ctx.pSimulator->GetSimDate();
			cRZAutoRefCount<cIGZDate> today, expiration;
			if (!pSimDate || !pSimDate->Clone(today.AsPPObj()) || !today || !today->Clone(expiration.AsPPObj()) || !expiration)
			{
				logger.WriteLine(LogLevel::Error, "MirrorDeals: could not clone the sim date, deal dropped.");
				continue;
			}
			expiration->operator+=(d.expiresIn);
			deal->SetCurrentDate(*today);
			deal->SetStartDate(*today);
			deal->SetExpirationDate(*expiration);
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: dates set, current=start=%u expiration=%u (+%d)",
				today->DayNumber(), expiration->DayNumber(), d.expiresIn);
			logger.WriteLine(LogLevel::Info, "MirrorDeals: AddDeal ...");
			logger.Flush();
			const bool added = ctx.pNeighborsSim->AddDeal(deal);
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: AddDeal=%d", added ? 1 : 0);
			if (!added)
			{
				continue;
			}
			SetAmountLogged(deal, d.amount);
			seenDeals[key] = { d.amount, typeB };
			result.created++;
		}
		CancelDeals(ctx, a, present, result);
		// Forget this publisher's deals it no longer lists, so a later deal
		// with the same commodity is mirrored again.
		for (auto it = seenDeals.begin(); it != seenDeals.end();)
		{
			const bool thisPublisher = std::get<1>(it->first) == a.regionX && std::get<2>(it->first) == a.regionZ;
			if (thisPublisher && a.dealsComplete && !listed.count(it->first)) it = seenDeals.erase(it);
			else ++it;
		}
		return result;
	}

	// Seen deals of this publisher it no longer lists or lists as cancelled.
	void NeighborMirror::CancelDeals(const MirrorContext& ctx, const NeighborSnapshot& a, const std::set<DealKey>& present, DealMirrorResult& result)
	{
		Logger& logger = Logger::GetInstance();
		std::vector<DealKey> gone;
		for (const auto& [key, seen] : seenDeals)
		{
			if (std::get<1>(key) == a.regionX && std::get<2>(key) == a.regionZ && !present.count(key)) gone.push_back(key);
		}
		if (gone.empty())
		{
			return;
		}
		if (!ListComplete(a, a.dealsComplete, "deal"))
		{
			return;
		}
		if (!RemovalAllowed(ctx, a, "deal", gone.size()))
		{
			for (const DealKey& key : gone) seenDeals.erase(key);
			return;
		}
		cISC4RegionalCity* pA = FindRegionalCity(ctx.pRegion, a.regionX, a.regionZ);
		for (const DealKey& key : gone)
		{
			const uint32_t localType = seenDeals[key].localType;
			seenDeals.erase(key);
			if (!pA)
			{
				logger.WriteLineFormatted(LogLevel::Info, "Removal: deal 0x%08X, no record for (%d,%d), key dropped.", localType, a.regionX, a.regionZ);
				continue;
			}
			SC4List<cISC4NeighborDeal*> deals;   // held across Cancel
			logger.WriteLineFormatted(LogLevel::Info, "Removal: deal 0x%08X no longer listed by (%d,%d). FindDeals ...", localType, a.regionX, a.regionZ);
			logger.Flush();
			ns641::FindDeals(ctx.pNeighborsSim, static_cast<int32_t>(localType), pA, deals);
			const size_t n = deals.size();
			if (n != 1)
			{
				logger.WriteLineFormatted(LogLevel::Info, "Removal: deal 0x%08X FindDeals count=%zu, not cancelled.", localType, n);
				continue;
			}
			cISC4NeighborDeal* deal = *deals.begin();
			logger.WriteLineFormatted(LogLevel::Info, "Removal: deal 0x%08X %p flags before=0x%X amount=%d. Cancel() ...",
				localType, static_cast<void*>(deal), static_cast<uint32_t>(deal->GetFlags()), deal->GetAmount());
			logger.Flush();
			deal->Cancel();
			// A status 2 sent inside Cancel is logged by the director's event handler above this line.
			logger.WriteLineFormatted(LogLevel::Info, "Removal: deal 0x%08X Cancel() returned, flags after=0x%X", localType, static_cast<uint32_t>(deal->GetFlags()));
			logger.Flush();
			result.cancelled++;
		}
	}
}
