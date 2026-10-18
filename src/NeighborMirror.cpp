/*
 * sc4-region-sync
 *
 * NeighborMirror.cpp
 *
 * Every game call here is new (docs/OPEN-QUESTIONS.md Q9), so each one is
 * logged at Info before and after, with a flush before the call.
 */

#include "NeighborMirror.h"
#include "NeighborsSim641.h"
#include "RegionalCityBridge.h"
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
	bool FindOwnTileSize(cISC4Region* pRegion, int32_t x, int32_t z, uint32_t& sizeEnum)
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
}

namespace regionsync
{
	void NeighborMirror::Reset()
	{
		mirroredConnections.clear();
		mirroredDeals.clear();
	}

	int NeighborMirror::MirrorConnections(const MirrorContext& ctx, const NeighborSnapshot& a)
	{
		Logger& logger = Logger::GetInstance();
		if (!ctx.pRegion || !ctx.pOwnRegionalCity || !ctx.pNeighborsSim)
		{
			logger.WriteLine(LogLevel::Error, "MirrorConnections: no region, city or neighbors simulator.");
			return 0;
		}
		if (!a.connectionsKnown)
		{
			logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: (%d,%d) snapshot has no connection list (schema 1?), skipped.", a.regionX, a.regionZ);
			return 0;
		}

		int32_t ownX = 0, ownZ = 0, sizeX = 0, sizeZ = 0;
		ctx.pOwnRegionalCity->GetPosition(ownX, ownZ);
		ctx.pOwnRegionalCity->GetCitySize(sizeX, sizeZ);
		uint32_t sizeEnum = 0;
		if (!FindOwnTileSize(ctx.pRegion, ownX, ownZ, sizeEnum))
		{
			logger.WriteLineFormatted(LogLevel::Error, "MirrorConnections: own tile (%d,%d) not in the region's location list, skipped.", ownX, ownZ);
			return 0;
		}
		const int32_t cells = CityCells(sizeEnum);
		logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: from (%d,%d) '%s', %zu connection(s). Own tile (%d,%d) GetCitySize=(%d,%d) sizeEnum=%u cells=%d",
			a.regionX, a.regionZ, a.cityName.c_str(), a.connections.size(), ownX, ownZ, sizeX, sizeZ, sizeEnum, cells);

		int created = 0;
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
			if (mirroredConnections.count(key))
			{
				continue;
			}

			const auto type = static_cast<cISC4NeighborsSimulator::ConnectionType>(c.type);
			logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: type %u inner=(%d,%d) -> our cell (%d,%d). HasConnection ...",
				c.type, c.innerX, c.innerZ, bx, bz);
			logger.Flush();
			const bool has = ctx.pNeighborsSim->HasConnection(type, bx, bz);
			logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: HasConnection=%d", has ? 1 : 0);
			if (has)
			{
				mirroredConnections.insert(key);
				continue;
			}

			logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: CreateConnection(%u, %d, %d, 0) ...", c.type, bx, bz);
			logger.Flush();
			// The return value is not set by the game (connections-641.md); FindConnection confirms.
			ctx.pNeighborsSim->CreateConnection(type, bx, bz, false);
			logger.WriteLine(LogLevel::Info, "MirrorConnections: CreateConnection returned");
			logger.Flush();
			cISC4NeighborConnection* pFound = ctx.pNeighborsSim->FindConnection(type, bx, bz);
			logger.WriteLineFormatted(LogLevel::Info, "MirrorConnections: FindConnection=%p", static_cast<void*>(pFound));
			logger.Flush();
			mirroredConnections.insert(key);
			if (pFound)
			{
				created++;
			}
		}
		return created;
	}

	int NeighborMirror::MirrorDeals(const MirrorContext& ctx, const NeighborSnapshot& a)
	{
		Logger& logger = Logger::GetInstance();
		if (!ctx.pRegion || !ctx.pOwnRegionalCity || !ctx.pNeighborsSim || !ctx.pSimulator)
		{
			logger.WriteLine(LogLevel::Error, "MirrorDeals: no region, city, neighbors simulator or simulator.");
			return 0;
		}
		if (!a.dealsKnown)
		{
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: (%d,%d) snapshot has no deal list (schema 1?), skipped.", a.regionX, a.regionZ);
			return 0;
		}

		int32_t ownX = 0, ownZ = 0;
		ctx.pOwnRegionalCity->GetPosition(ownX, ownZ);
		logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: from (%d,%d) '%s', %zu deal(s).", a.regionX, a.regionZ, a.cityName.c_str(), a.deals.size());

		int created = 0;
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
			if (mirroredDeals.count(key))
			{
				continue;
			}

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
					mirroredDeals.insert(key);
					continue;
				}
			}

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
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: SetAmount(%d) ...", d.amount);
			logger.Flush();
			deal->SetAmount(d.amount);
			logger.WriteLineFormatted(LogLevel::Info, "MirrorDeals: SetAmount returned, amount=%d cost=%lld flags=0x%X",
				deal->GetAmount(), deal->GetCost(), static_cast<uint32_t>(deal->GetFlags()));
			logger.Flush();
			mirroredDeals.insert(key);
			created++;
		}
		return created;
	}
}
