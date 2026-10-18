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
	// into pA's +0x90 list. Returns true when FindConnection then finds it.
	bool InjectRecordConnection(const regionsync::MirrorContext& ctx, cISC4RegionalCity* pA, const regionsync::ConnectionRecord& c)
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
			return false;
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
			return false;
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
			return false;
		}
		return true;
	}

	// The record-side step (record-connections-641.md): make sure the
	// publisher's record copy in this process holds the connection at its
	// cell, active, so FindConnectionOnOtherSide and the buy-side deal check
	// find it. Injection only when ctx.allowRecordInjection. Returns true
	// when a new object was linked in. listState: 0 = not checked this call,
	// 1 = ok, 2 = check failed.
	bool EnsureRecordConnection(const regionsync::MirrorContext& ctx, int32_t aX, int32_t aZ, int32_t aCells,
		const regionsync::ConnectionRecord& c, cISC4NeighborConnection* bConn, int& listState)
	{
		namespace rc641 = regionsync::rc641;
		Logger& logger = Logger::GetInstance();
		if (!c.cellKnown || !regionsync::IsConnectionCellAdjacent(c.innerX, c.innerZ, c.cellX, c.cellZ, aCells))
		{
			logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: type %u inner=(%d,%d) cell=(%d,%d) known=%d cellsA=%d not usable, skipped.",
				c.type, c.innerX, c.innerZ, c.cellX, c.cellZ, c.cellKnown ? 1 : 0, aCells);
			return false;
		}
		// ReloadCity replaces record objects: resolve every time.
		cISC4RegionalCity* pA = regionsync::FindRegionalCity(ctx.pRegion, aX, aZ);
		if (!pA)
		{
			logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: no record for (%d,%d), skipped.", aX, aZ);
			return false;
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

		bool injected = false;
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
				return false;
			}
			if (!InjectRecordConnection(ctx, pA, c))
			{
				return false;
			}
			injected = true;
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
		mirroredConnections.clear();
		edgeLayTried.clear();
		seenDeals.clear();
	}

	ConnectionMirrorResult NeighborMirror::MirrorConnections(const MirrorContext& ctx, const NeighborSnapshot& a)
	{
		Logger& logger = Logger::GetInstance();
		ConnectionMirrorResult result;
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
		logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: from (%d,%d) '%s', %zu connection(s). Own tile (%d,%d) GetCitySize=(%d,%d) sizeEnum=%u cells=%d",
			a.regionX, a.regionZ, a.cityName.c_str(), a.connections.size(), ownX, ownZ, sizeX, sizeZ, sizeEnum, cells);

		int listState = 0;
		for (const ConnectionRecord& c : a.connections)
		{
			if (c.otherTileX != ownX || c.otherTileZ != ownZ)
			{
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
			const ConnectionKey key{ a.regionX, a.regionZ, c.type, c.innerX, c.innerZ };
			if (!mirroredConnections.count(key))
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
					logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: CreateConnection(%u, %d, %d, 0) ...", c.type, bx, bz);
					logger.Flush();
					// The return value is not set by the game (connections-641.md); FindConnection confirms.
					ctx.pNeighborsSim->CreateConnection(type, bx, bz, false);
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
					if (EnsureRecordConnection(ctx, a.regionX, a.regionZ, aCells, c, pFound, listState))
					{
						result.injected++;
					}
					logger.WriteLineFormatted(LogLevel::Info, "RecordConnection: own GetAllConnections count after=%zu", OwnConnectionCount(ctx.pNeighborsSim));
				}
				mirroredConnections.insert(key);
			}

			// Idempotent, so it runs for every connection, not only new ones;
			// once per key per session on the timed path, since a failed lay
			// has already demolished the cells.
			if (ctx.layEdgeNetwork && (ctx.allowRecordInjection || !edgeLayTried.count(key)))
			{
				edgeLayTried.insert(key);
				const auto type = static_cast<cISC4NeighborsSimulator::ConnectionType>(c.type);
				cISC4NeighborConnection* pConn = ctx.pNeighborsSim->FindConnection(type, bx, bz);
				logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: FindConnection(%u, %d, %d)=%p", c.type, bx, bz, static_cast<void*>(pConn));
				if (pConn)
				{
					const EdgeLayResult lay = LayEdgePiece(ctx, c.type, bx, bz, cells);
					logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: type %u (%d,%d) laid=%d (%s)", c.type, bx, bz, lay.laid ? 1 : 0, lay.reason);
					if (lay.laid) result.laid++;
				}
			}
		}
		return result;
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
		for (const DealRecord& d : a.deals)
		{
			if (d.otherTileX != ownX || d.otherTileZ != ownZ)
			{
				continue;
			}
			const uint32_t flags = MaskDealFlags(d.flags);
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
			const bool changed = !seenBefore || seen->second != d.amount;

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
								seenDeals[key] = d.amount;
							}
						}
						else
						{
							logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: existing deal seen (%zu match), amount=%d recorded.", n, d.amount);
							seenDeals[key] = d.amount;
						}
					}
					else if (changed && n == 1)
					{
						// Not recorded when the follow was refused, so it is retried.
						if (FollowAmount(*existing.begin(), d.amount, result))
						{
							seenDeals[key] = d.amount;
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
			seenDeals[key] = d.amount;
			result.created++;
		}
		// Forget this publisher's deals it no longer lists, so a later deal
		// with the same commodity is mirrored again.
		for (auto it = seenDeals.begin(); it != seenDeals.end();)
		{
			const bool thisPublisher = std::get<1>(it->first) == a.regionX && std::get<2>(it->first) == a.regionZ;
			if (thisPublisher && !listed.count(it->first)) it = seenDeals.erase(it);
			else ++it;
		}
		return result;
	}
}
