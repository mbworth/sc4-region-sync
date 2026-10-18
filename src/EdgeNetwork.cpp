/*
 * sc4-region-sync
 *
 * EdgeNetwork.cpp
 *
 * First exercise of the network tool and occupant queries (Q10): every game
 * call is logged at Info before and after, with a flush before the call.
 */

#include "EdgeNetwork.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4City.h"
#include "cISC4Demolition.h"
#include "cISC4NetworkManager.h"
#include "cISC4NetworkOccupant.h"
#include "cISC4NetworkTool.h"
#include "cISC4Occupant.h"
#include "cISC4OccupantManager.h"
#include "cISC4ZoneManager.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cSC4BaseOccupantFilter.h"
#include "SC4CellRegion.h"
#include "SC4List.h"
#include "SC4Point.h"
#include "Logger.h"
#include <algorithm>
#include <functional>

namespace
{
	using NetType = cISC4NetworkOccupant::eNetworkType;
	constexpr uint32_t kOccupantTypeNetwork = 0xC772BF98;
	constexpr uint32_t kOccupantTypePowerPole = 0x2890D4DE;
	constexpr uint32_t kOccupantTypePowerWire = 0x0990B2EA;
	constexpr uint32_t kAnyOccupantType = 0;   // typed list query: 0 means no filter
	constexpr uint32_t kAllMatches = 0xFFFFFFFF;

	Logger& Log() { return Logger::GetInstance(); }

	// Runs fn on each occupant of type `type` on one standard cell, then ReleaseOccupantList.
	void ForEachOccupant(cISC4OccupantManager* occ, int32_t x, int32_t z, uint32_t type, const std::function<void(cISC4Occupant*)>& fn)
	{
		SC4List<cISC4Occupant*> list;
		const SC4Point<int> cell(x, z);
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: GetOccupantsByStandardCityCells((%d,%d), 0x%08X) ...", x, z, type);
		Log().Flush();
		const bool ok = occ->GetOccupantsByStandardCityCells(list, &cell, &cell, type, kAllMatches);
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: GetOccupantsByStandardCityCells=%d count=%zu", ok ? 1 : 0, list.size());
		for (cISC4Occupant* o : list)
		{
			if (o) fn(o);
		}
		occ->ReleaseOccupantList(list);
		Log().WriteLine(LogLevel::Info, "EdgeTile: ReleaseOccupantList returned");
	}

	bool IsNetworkOfType(cISC4Occupant* o, NetType t)
	{
		cRZAutoRefCount<cISC4NetworkOccupant> no;
		return o->QueryInterface(GZIID_cISC4NetworkOccupant, no.AsPPVoid()) && no && no->IsOfType(t);
	}

	// Network pieces of netType on the cell; power counts poles.
	int CountPieces(cISC4OccupantManager* occ, int32_t x, int32_t z, NetType netType)
	{
		const bool power = netType == NetType::PowerPole;
		int n = 0;
		ForEachOccupant(occ, x, z, power ? kOccupantTypePowerPole : kOccupantTypeNetwork, [&](cISC4Occupant* o) {
			if (power)
			{
				n++;
				return;
			}
			if (IsNetworkOfType(o, netType)) n++;
		});
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: cell (%d,%d) pieces of network %u = %d", x, z, static_cast<uint32_t>(netType), n);
		return n;
	}

	bool HasOccupantOfType(cISC4OccupantManager* occ, int32_t x, int32_t z, uint32_t type)
	{
		cISC4Occupant* raw = nullptr;
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: GetFirstOccupantByStandardCityCell((%d,%d), 0x%08X) ...", x, z, type);
		Log().Flush();
		const bool hit = occ->GetFirstOccupantByStandardCityCell(raw, x, z, type);
		const cRZAutoRefCount<cISC4Occupant> held(raw);   // a hit carries one reference
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: GetFirstOccupantByStandardCityCell=%d occupant=%p", hit ? 1 : 0, static_cast<void*>(raw));
		return hit && raw;
	}

	// Logs every occupant on the cell; returns the count.
	int LogOccupants(cISC4OccupantManager* occ, int32_t x, int32_t z, const char* when)
	{
		int n = 0;
		ForEachOccupant(occ, x, z, kAnyOccupantType, [&](cISC4Occupant* o) {
			n++;
			Log().WriteLineFormatted(LogLevel::Info, "EdgeTile:   %s (%d,%d) occupant %p type=0x%08X", when, x, z, static_cast<void*>(o), static_cast<uint32_t>(o->GetType()));
		});
		return n;
	}

	// The game's layer filter (research step 6): pipe and subway networks
	// demolish only their own layer, everything else only the surface.
	class LayerFilter final : public cSC4BaseOccupantFilter
	{
	public:
		explicit LayerFilter(NetType netType) : netType(netType) {}
		bool IsOccupantIncluded(cISC4Occupant* o) override
		{
			if (!o) return false;
			if (netType == NetType::WaterPipe || netType == NetType::Subway) return IsNetworkOfType(o, netType);
			return !IsNetworkOfType(o, NetType::WaterPipe) && !IsNetworkOfType(o, NetType::Subway);
		}
	private:
		NetType netType;
	};

	long long Funds(cISC4City* pCity)
	{
		cISC4BudgetSimulator* b = pCity->GetBudgetSimulator();
		return b ? static_cast<long long>(b->GetTotalFunds()) : -1;
	}

	// Private tool instance: Shutdown and Release on every exit.
	struct ToolGuard
	{
		cISC4NetworkTool* tool;
		explicit ToolGuard(cISC4NetworkTool* t) : tool(t) { tool->AddRef(); }
		~ToolGuard()
		{
			Log().WriteLine(LogLevel::Info, "EdgeTile: tool Shutdown ...");
			Log().Flush();
			const bool ok = tool->Shutdown();
			Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: Shutdown=%d; Release ...", ok ? 1 : 0);
			Log().Flush();
			const uint32_t refs = tool->Release();
			Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: Release=%u", refs);
		}
		ToolGuard(const ToolGuard&) = delete;
		ToolGuard& operator=(const ToolGuard&) = delete;
	};

	bool Place(cISC4NetworkTool* tool, const regionsync::EdgeStub& s, const char* pass)
	{
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: PlaceNetworkOccupants(%d,%d -> %d,%d, 0, null, true, -1) %s ...", s.sx, s.sz, s.ex, s.ez, pass);
		Log().Flush();
		const uint32_t placed = tool->PlaceNetworkOccupants(
			static_cast<uint32_t>(s.sx), static_cast<uint32_t>(s.sz), static_cast<uint32_t>(s.ex), static_cast<uint32_t>(s.ez),
			0, nullptr, true, static_cast<NetType>(0xFFFFFFFFu));
		const uint32_t failure = tool->GetFailureState();
		cRZBaseString reason;
		tool->GetFailureReasonString(reason);
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: PlaceNetworkOccupants=%u GetFailureState=%u reason='%s'", placed & 0xFF, failure, reason.ToChar());
		Log().Flush();
		return (placed & 0xFF) != 0;
	}
}

namespace regionsync
{
	EdgeLayResult LayEdgePiece(const MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, int32_t cells)
	{
		Logger& logger = Log();
		const int32_t net = ConnectionToNetworkType(connType);
		if (net < 0)
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: type %u has no single network, not laid.", connType);
			return { false, "unsupported type" };
		}
		cISC4City* pCity = ctx.pCity;
		cISC4OccupantManager* occ = pCity ? pCity->GetOccupantManager() : nullptr;
		cISC4ZoneManager* zones = pCity ? pCity->GetZoneManager() : nullptr;
		cISC4NetworkManager* nm = pCity ? pCity->GetNetworkManager() : nullptr;
		if (!occ || !zones || !nm)
		{
			logger.WriteLineFormatted(LogLevel::Error, "EdgeTile: city=%p occupants=%p zones=%p network=%p, not laid.",
				static_cast<void*>(pCity), static_cast<void*>(occ), static_cast<void*>(zones), static_cast<void*>(nm));
			return { false, "no managers" };
		}

		const int32_t countX = static_cast<int32_t>(pCity->CellCountX());
		const int32_t countZ = static_cast<int32_t>(pCity->CellCountZ());
		int32_t n = cells;
		if (countX != cells || countZ != cells)
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: CellCountX/Z=(%d,%d) differs from cells=%d.", countX, countZ, cells);
			if (countX == countZ && countX > 0) n = countX;
		}
		const std::optional<EdgeStub> stub = EdgeStubGeometry(bx, bz, n);
		if (!stub)
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: cell (%d,%d) is not just outside a %d-cell edge, not laid.", bx, bz, n);
			return { false, "geometry" };
		}
		const EdgeStub& s = *stub;
		const NetType netType = static_cast<NetType>(net);
		const int32_t dx = (s.ex > s.sx) - (s.ex < s.sx);
		const int32_t dz = (s.ez > s.sz) - (s.ez < s.sz);
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: type %u net %d connection (%d,%d) side %u stub (%d,%d) -> (%d,%d)",
			connType, net, bx, bz, s.side, s.sx, s.sz, s.ex, s.ez);

		if (CountPieces(occ, s.sx, s.sz, netType) > 0)
		{
			logger.WriteLine(LogLevel::Info, "EdgeTile: already laid.");
			return { false, "already laid" };
		}
		// Region over the 3 cells; a cell that already holds a network (power:
		// pole or wire) is left out of the demolition, as the game does.
		const int32_t x1 = std::min(s.sx, s.ex), z1 = std::min(s.sz, s.ez);
		SC4CellRegion<int32_t> region(x1, z1, std::max(s.sx, s.ex), std::max(s.sz, s.ez), true);
		const bool power = netType == NetType::PowerPole;
		for (int32_t i = 0; i < 3; i++)
		{
			const int32_t cx = s.sx + i * dx, cz = s.sz + i * dz;
			const int count = LogOccupants(occ, cx, cz, "before");
			// Power also keeps any network cell (a road crossing the stub line).
			const bool network = HasOccupantOfType(occ, cx, cz, kOccupantTypeNetwork)
				|| (power && (HasOccupantOfType(occ, cx, cz, kOccupantTypePowerPole) || HasOccupantOfType(occ, cx, cz, kOccupantTypePowerWire)));
			if (network)
			{
				region.cellMap.SetValue(static_cast<uint32_t>(cx - x1), static_cast<uint32_t>(cz - z1), false);
			}
			const auto zone = zones->GetZoneType(cx, cz);
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: cell (%d,%d) occupants=%d network=%d zone=%d -> %s",
				cx, cz, count, network ? 1 : 0, static_cast<int32_t>(zone), network ? "kept" : "demolish");
		}

		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: GetNetworkTool(%d, true) ...", net);
		logger.Flush();
		cISC4NetworkTool* rawTool = nm->GetNetworkTool(net, true);
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: GetNetworkTool=%p", static_cast<void*>(rawTool));
		if (!rawTool)
		{
			return { false, "no tool" };
		}
		const ToolGuard guard(rawTool);
		cISC4NetworkTool* tool = guard.tool;
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: funds before=%lld", Funds(pCity));

		cISC4Demolition* demolition = pCity->GetDemolitionUtility();
		if (demolition)
		{
			const cRZAutoRefCount<cISC4OccupantFilter> filter(new LayerFilter(netType), cRZAutoRefCount<cISC4OccupantFilter>::kAddRef);
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: DemolishRegion(true, (%d,%d)-(%d,%d), 0, 1, false, layer filter, null, 0, null, 0, 0) ...",
				region.bounds.topLeftX, region.bounds.topLeftY, region.bounds.bottomRightX, region.bounds.bottomRightY);
			logger.Flush();
			const bool demolished = demolition->DemolishRegion(true, region, 0, 1, false, filter, nullptr, 0, nullptr, 0, 0);
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: DemolishRegion=%d funds=%lld", demolished ? 1 : 0, Funds(pCity));
			for (int32_t i = 0; i < 3; i++)
			{
				LogOccupants(occ, s.sx + i * dx, s.sz + i * dz, "after demolition");
			}
		}
		else
		{
			logger.WriteLine(LogLevel::Error, "EdgeTile: no demolition utility, placing without demolition.");
		}

		logger.WriteLine(LogLevel::Info, "EdgeTile: Init ...");
		logger.Flush();
		const bool inited = tool->Init();
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: Init=%d", inited ? 1 : 0);

		if (power)
		{
			logger.WriteLine(LogLevel::Info, "EdgeTile: PlaceZone(region, None, true, true, false, true, false, null, null, 0) ...");
			logger.Flush();
			const bool dezoned = zones->PlaceZone(region, cISC4ZoneManager::ZoneType::None, true, true, false, true, false, nullptr, nullptr, 0);
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: PlaceZone=%d", dezoned ? 1 : 0);
		}

		Place(tool, s, "first");
		int pieces[3] = {};
		for (int32_t i = 0; i < 3; i++)
		{
			pieces[i] = CountPieces(occ, s.sx + i * dx, s.sz + i * dz, netType);
		}
		if (pieces[0] == 0)
		{
			logger.WriteLine(LogLevel::Info, "EdgeTile: no piece on the edge cell.");
			return { false, "no piece" };
		}

		if (!power)
		{
			const uint32_t clear = ~(0xFFu << (s.side * 8));
			const uint32_t set = 2u << (s.side * 8);
			ForEachOccupant(occ, s.sx, s.sz, kOccupantTypeNetwork, [&](cISC4Occupant* o) {
				cRZAutoRefCount<cISC4NetworkOccupant> no;
				if (!o->QueryInterface(GZIID_cISC4NetworkOccupant, no.AsPPVoid()) || !no || !no->IsOfType(netType)) return;
				const uint32_t f = no->GetEdgeTypesFlag(netType);
				const uint32_t nf = (f & clear) | set;
				logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: %p GetEdgeTypesFlag=0x%08X (0x04 bits 0x%08X). SetEdgeTypesFlag(0x%08X) ...",
					static_cast<void*>(no), f, f & 0x04040404u, nf);
				logger.Flush();
				no->SetEdgeTypesFlag(netType, nf);
				logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: SetEdgeTypesFlag returned, now 0x%08X", no->GetEdgeTypesFlag(netType));
			});
		}

		Place(tool, s, "second");
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: funds after=%lld", Funds(pCity));
		for (int32_t i = 0; i < 3; i++)
		{
			pieces[i] = CountPieces(occ, s.sx + i * dx, s.sz + i * dz, netType);
		}
		const bool edge = pieces[0] > 0;
		const bool full = edge && pieces[1] > 0 && pieces[2] > 0;
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: result cells=%d,%d,%d -> %s", pieces[0], pieces[1], pieces[2],
			full ? "laid" : edge ? "partial (inner cells missing)" : "edge piece gone after second placement");
		return { edge, full ? "laid" : edge ? "partial" : "no piece" };
	}
}
