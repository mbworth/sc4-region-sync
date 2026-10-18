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
#include "cISCPropertyHolder.h"
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
	constexpr uint32_t kConnectionMarkerProperty = 0xCAA46595;

	// Logs every occupant on the cell (marker props flagged); returns the count.
	int LogOccupants(cISC4OccupantManager* occ, int32_t x, int32_t z, const char* when)
	{
		int n = 0;
		ForEachOccupant(occ, x, z, kAnyOccupantType, [&](cISC4Occupant* o) {
			n++;
			cISCPropertyHolder* p = o->AsPropertyHolder();
			const bool marker = p && p->HasProperty(kConnectionMarkerProperty);
			Log().WriteLineFormatted(LogLevel::Info, "EdgeTile:   %s (%d,%d) occupant %p type=0x%08X%s", when, x, z, static_cast<void*>(o), static_cast<uint32_t>(o->GetType()), marker ? " marker" : "");
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
			// The connection's arrow marker (property 0xCAA46595, research) sits on
			// the edge cell; the game lays it after demolition, we already have it.
			if (cISCPropertyHolder* p = o->AsPropertyHolder(); p && p->HasProperty(kConnectionMarkerProperty))
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "EdgeTile: occupant %p type=0x%08X is the connection marker, kept.", static_cast<void*>(o), static_cast<uint32_t>(o->GetType()));
				return false;
			}
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

namespace
{
	struct EdgeSetup
	{
		cISC4City* city = nullptr;
		cISC4OccupantManager* occ = nullptr;
		cISC4ZoneManager* zones = nullptr;
		cISC4NetworkManager* nm = nullptr;
		int32_t net = -1;
		int32_t cells = 0;   // city width from CellCountX/Z when they agree
		regionsync::EdgeStub stub;
	};

	// Managers, network type and stub geometry; reason is set on failure.
	bool SetUpEdge(const regionsync::MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, int32_t cells, EdgeSetup& e, const char*& reason)
	{
		Logger& logger = Log();
		e.net = regionsync::ConnectionToNetworkType(connType);
		if (e.net < 0)
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: type %u has no single network.", connType);
			reason = "unsupported type";
			return false;
		}
		e.city = ctx.pCity;
		e.occ = e.city ? e.city->GetOccupantManager() : nullptr;
		e.zones = e.city ? e.city->GetZoneManager() : nullptr;
		e.nm = e.city ? e.city->GetNetworkManager() : nullptr;
		if (!e.occ || !e.zones || !e.nm)
		{
			logger.WriteLineFormatted(LogLevel::Error, "EdgeTile: city=%p occupants=%p zones=%p network=%p.",
				static_cast<void*>(e.city), static_cast<void*>(e.occ), static_cast<void*>(e.zones), static_cast<void*>(e.nm));
			reason = "no managers";
			return false;
		}

		const int32_t countX = static_cast<int32_t>(e.city->CellCountX());
		const int32_t countZ = static_cast<int32_t>(e.city->CellCountZ());
		e.cells = cells;
		if (countX != cells || countZ != cells)
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: CellCountX/Z=(%d,%d) differs from cells=%d.", countX, countZ, cells);
			if (countX == countZ && countX > 0) e.cells = countX;
		}
		const std::optional<regionsync::EdgeStub> stub = regionsync::EdgeStubGeometry(bx, bz, e.cells);
		if (!stub)
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: cell (%d,%d) is not just outside a %d-cell edge.", bx, bz, e.cells);
			reason = "geometry";
			return false;
		}
		e.stub = *stub;
		return true;
	}

	// SetEdgeTypesFlag(net, update(f)) on each piece of net on the cell; returns the OR of the old flags.
	// skipUnchanged: no Set when update(f) == f.
	uint32_t UpdateEdgeFlags(cISC4OccupantManager* occ, int32_t x, int32_t z, NetType netType, const std::function<uint32_t(uint32_t)>& update,
		bool skipUnchanged = false)
	{
		uint32_t seen = 0;
		ForEachOccupant(occ, x, z, kOccupantTypeNetwork, [&](cISC4Occupant* o) {
			cRZAutoRefCount<cISC4NetworkOccupant> no;
			if (!o->QueryInterface(GZIID_cISC4NetworkOccupant, no.AsPPVoid()) || !no || !no->IsOfType(netType)) return;
			const uint32_t f = no->GetEdgeTypesFlag(netType);
			const uint32_t nf = update(f);
			seen |= f;
			if (skipUnchanged && nf == f)
			{
				Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: %p GetEdgeTypesFlag=0x%08X (0x04 bits 0x%08X), unchanged.",
					static_cast<void*>(no), f, f & 0x04040404u);
				return;
			}
			Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: %p GetEdgeTypesFlag=0x%08X (0x04 bits 0x%08X). SetEdgeTypesFlag(0x%08X) ...",
				static_cast<void*>(no), f, f & 0x04040404u, nf);
			Log().Flush();
			no->SetEdgeTypesFlag(netType, nf);
			Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: SetEdgeTypesFlag returned, now 0x%08X", no->GetEdgeTypesFlag(netType));
		});
		return seen;
	}

	// 0x04 in byte i: the piece continues one cell west, north, east, south.
	constexpr int32_t kStepX[4] = { -1, 0, 1, 0 };
	constexpr int32_t kStepZ[4] = { 0, -1, 0, 1 };

	// One cell toward the higher coordinate along the edge: +z on a west or
	// east edge, +x on a north or south edge.
	struct Step
	{
		int32_t x, z;
	};
	constexpr Step AlongStep(uint32_t side) { return (side == 1 || side == 3) ? Step{ 1, 0 } : Step{ 0, 1 }; }
	// Flag byte toward the lower along-edge neighbour (north or west); toward the higher one is +2.
	constexpr uint32_t TowardLower(uint32_t side) { return AlongStep(side).x ? 0 : 1; }

	// Side byte to 2 on hi's edge cell, and on lo's when the 0x04 bit links
	// the two pieces. alreadyLaid: leave a piece whose flags already match,
	// and one with no inward link (a network along the edge, not a stub).
	// 0x04 is the dual-lane partner link, 0x02 a plain link (log, 2026-10-18).
	void SetSideFlags(cISC4OccupantManager* occ, const regionsync::EdgeStub& hi, const regionsync::EdgeStub* lo, NetType netType, bool alreadyLaid)
	{
		const uint32_t clear = ~(0xFFu << (hi.side * 8));
		const uint32_t set = 2u << (hi.side * 8);
		const uint32_t inward = (hi.side + 2) & 3;
		const auto mark = [&](uint32_t f) {
			if (alreadyLaid && !((f >> (inward * 8)) & 0xFFu))
			{
				Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: flags 0x%08X have no inward link, not a stub, flag skipped.", f);
				return f;
			}
			return (f & clear) | set;
		};
		uint32_t loFlags = 0;
		if (lo)
		{
			ForEachOccupant(occ, lo->sx, lo->sz, kOccupantTypeNetwork, [&](cISC4Occupant* o) {
				cRZAutoRefCount<cISC4NetworkOccupant> no;
				if (IsNetworkOfType(o, netType) && o->QueryInterface(GZIID_cISC4NetworkOccupant, no.AsPPVoid()) && no) loFlags |= no->GetEdgeTypesFlag(netType);
			});
		}
		const uint32_t hiFlags = UpdateEdgeFlags(occ, hi.sx, hi.sz, netType, mark, alreadyLaid);
		if (!lo) return;
		const uint32_t down = TowardLower(hi.side);
		const bool linked = ((hiFlags >> (down * 8)) & 0x04u) || ((loFlags >> ((down + 2) * 8)) & 0x04u);
		Log().WriteLineFormatted(LogLevel::Info, "EdgeTile: pair link hi (%d,%d) flags 0x%08X, lo (%d,%d) flags 0x%08X -> %s",
			hi.sx, hi.sz, hiFlags, lo->sx, lo->sz, loFlags, linked ? "linked, lo flag set" : "not linked, lo flag skipped");
		if (linked) UpdateEdgeFlags(occ, lo->sx, lo->sz, netType, mark, alreadyLaid);
	}

	// Pieces of net on an edge cell next to the stub block; -1 off the map.
	int OutsideRow(cISC4OccupantManager* occ, int32_t x, int32_t z, int32_t cells, NetType netType, const char* prefix, const char* row, const char* when)
	{
		const int n = (x < 0 || z < 0 || x >= cells || z >= cells) ? -1 : CountPieces(occ, x, z, netType);
		Log().WriteLineFormatted(LogLevel::Info, "%s: row %s edge cell (%d,%d) pieces=%d %s", prefix, row, x, z, n, when);
		return n;
	}

	// One stub (lo null) or a dual pair laid from hi's line (docs/PLAN-DUAL-FERRY.md rule 8).
	regionsync::EdgeLayResult LayEdge(const regionsync::MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, const SC4Point<int32_t>* loCell, int32_t cells)
	{
		using regionsync::EdgeStub;
		Logger& logger = Log();
		EdgeSetup e;
		const char* reason = "";
		if (!SetUpEdge(ctx, connType, bx, bz, cells, e, reason))
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: not laid (%s).", reason);
			return { false, reason };
		}
		cISC4City* pCity = e.city;
		cISC4OccupantManager* occ = e.occ;
		cISC4ZoneManager* zones = e.zones;
		cISC4NetworkManager* nm = e.nm;
		const int32_t net = e.net;
		const EdgeStub& s = e.stub;
		const NetType netType = static_cast<NetType>(net);
		const int32_t dx = (s.ex > s.sx) - (s.ex < s.sx);
		const int32_t dz = (s.ez > s.sz) - (s.ez < s.sz);
		const bool power = netType == NetType::PowerPole;
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: type %u net %d connection (%d,%d) side %u stub (%d,%d) -> (%d,%d)",
			connType, net, bx, bz, s.side, s.sx, s.sz, s.ex, s.ez);

		// Pair: lo is one cell lower along the same edge (ax, az = along-edge step).
		const bool pair = loCell != nullptr;
		const Step along = AlongStep(s.side);
		const int32_t ax = along.x, az = along.z;
		EdgeStub lo{};
		if (pair)
		{
			const std::optional<EdgeStub> loStub = regionsync::EdgeStubGeometry(loCell->x, loCell->y, e.cells);
			if (loCell->x != bx - ax || loCell->y != bz - az || !loStub || loStub->side != s.side)
			{
				logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair lo connection (%d,%d) is not one cell below hi (%d,%d) on the same edge, not laid.",
					loCell->x, loCell->y, bx, bz);
				return { false, "geometry" };
			}
			lo = *loStub;
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair lo connection (%d,%d) stub (%d,%d) -> (%d,%d)", loCell->x, loCell->y, lo.sx, lo.sz, lo.ex, lo.ez);
		}
		const int nLines = pair ? 2 : 1;
		const EdgeStub lines[2] = { s, lo };

		if (CountPieces(occ, s.sx, s.sz, netType) > 0)
		{
			logger.WriteLine(LogLevel::Info, "EdgeTile: already laid.");
			for (int l = 0; l < nLines; l++) LogOccupants(occ, lines[l].sx, lines[l].sz, "now");
			if (!power) SetSideFlags(occ, s, pair ? &lo : nullptr, netType, true);
			return { false, "already laid" };
		}
		if (pair && CountPieces(occ, lo.sx, lo.sz, netType) > 0)
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: only lo's edge cell (%d,%d) holds a piece (half stub), not laid.", lo.sx, lo.sz);
			return { false, "half stub" };
		}
		// Region over the 3 (pair: 6) cells; a cell that already holds a network
		// (power: pole or wire) is left out of the demolition, as the game does.
		int32_t x1 = s.sx, z1 = s.sz, x2 = s.sx, z2 = s.sz;
		for (int l = 0; l < nLines; l++)
		{
			x1 = std::min({ x1, lines[l].sx, lines[l].ex });
			z1 = std::min({ z1, lines[l].sz, lines[l].ez });
			x2 = std::max({ x2, lines[l].sx, lines[l].ex });
			z2 = std::max({ z2, lines[l].sz, lines[l].ez });
		}
		SC4CellRegion<int32_t> region(x1, z1, x2, z2, true);
		for (int l = 0; l < nLines; l++)
		{
			for (int32_t i = 0; i < 3; i++)
			{
				const int32_t cx = lines[l].sx + i * dx, cz = lines[l].sz + i * dz;
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
		}
		// Rows lo-1 and hi+1 must not change: the width-2 tool could widen into them.
		const int32_t loOutX = lo.sx - ax, loOutZ = lo.sz - az, hiOutX = s.sx + ax, hiOutZ = s.sz + az;
		const auto outside = [&](bool low, const char* when) {
			return low ? OutsideRow(occ, loOutX, loOutZ, e.cells, netType, "EdgeTile", "lo-1", when)
				: OutsideRow(occ, hiOutX, hiOutZ, e.cells, netType, "EdgeTile", "hi+1", when);
		};
		int outLo = 0, outHi = 0;
		if (pair)
		{
			outLo = outside(true, "before");
			outHi = outside(false, "before");
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
			for (int l = 0; l < nLines; l++)
			{
				for (int32_t i = 0; i < 3; i++)
				{
					LogOccupants(occ, lines[l].sx + i * dx, lines[l].sz + i * dz, "after demolition");
				}
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

		int pieces[2][3] = {};
		const auto count = [&] {
			for (int l = 0; l < nLines; l++)
			{
				for (int32_t i = 0; i < 3; i++)
				{
					pieces[l][i] = CountPieces(occ, lines[l].sx + i * dx, lines[l].sz + i * dz, netType);
				}
			}
		};
		Place(tool, s, "first");
		count();
		if (pieces[0][0] == 0)
		{
			logger.WriteLine(LogLevel::Info, "EdgeTile: no piece on the edge cell.");
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: funds after=%lld", Funds(pCity));
			return { false, "no piece" };
		}
		if (pair)
		{
			const int outLoNow = outside(true, "after first placement");
			const int outHiNow = outside(false, "after first placement");
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair first placement hi cells=%d,%d,%d lo cells=%d,%d,%d outside lo-1 %d -> %d, hi+1 %d -> %d",
				pieces[0][0], pieces[0][1], pieces[0][2], pieces[1][0], pieces[1][1], pieces[1][2], outLo, outLoNow, outHi, outHiNow);
			const char* stop = pieces[1][0] + pieces[1][1] + pieces[1][2] == 0 ? "lo row empty"
				: pieces[1][0] == 0 ? "lo edge cell empty"
				: (outLoNow != outLo || outHiNow != outHi) ? "outside row changed" : nullptr;
			if (stop)
			{
				logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair %s; flags and second placement skipped, both connections left.", stop);
				logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: funds after=%lld", Funds(pCity));
				return { false, stop };
			}
		}

		if (!power)
		{
			SetSideFlags(occ, s, pair ? &lo : nullptr, netType, false);
		}

		Place(tool, s, "second");
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: funds after=%lld", Funds(pCity));
		count();
		if (!pair)
		{
			const bool edge = pieces[0][0] > 0;
			const bool full = edge && pieces[0][1] > 0 && pieces[0][2] > 0;
			logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: result cells=%d,%d,%d -> %s", pieces[0][0], pieces[0][1], pieces[0][2],
				full ? "laid" : edge ? "partial (inner cells missing)" : "edge piece gone after second placement");
			return { edge, full ? "laid" : edge ? "partial" : "no piece" };
		}
		const int outLoEnd = outside(true, "after second placement");
		const int outHiEnd = outside(false, "after second placement");
		const bool laid = pieces[0][0] > 0 && pieces[1][0] > 0;
		const bool full = laid && pieces[0][1] > 0 && pieces[0][2] > 0 && pieces[1][1] > 0 && pieces[1][2] > 0;
		const bool outsideChanged = outLoEnd != outLo || outHiEnd != outHi;
		const char* result = !laid ? "edge piece missing" : !full ? "partial" : outsideChanged ? "laid, outside row changed" : "laid";
		logger.WriteLineFormatted(LogLevel::Info, "EdgeTile: pair result hi cells=%d,%d,%d lo cells=%d,%d,%d outside lo-1 %d -> %d, hi+1 %d -> %d -> %s",
			pieces[0][0], pieces[0][1], pieces[0][2], pieces[1][0], pieces[1][1], pieces[1][2], outLo, outLoEnd, outHi, outHiEnd,
			!laid ? "an edge piece is missing after second placement" : !full ? "partial (inner cells missing)" : result);
		return { laid, result };
	}
}

namespace regionsync
{
	EdgeLayResult LayEdgePiece(const MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, int32_t cells)
	{
		return LayEdge(ctx, connType, bx, bz, nullptr, cells);
	}

	EdgeLayResult LayEdgePair(const MirrorContext& ctx, uint32_t connType, int32_t loX, int32_t loZ, int32_t hiX, int32_t hiZ, int32_t cells)
	{
		const SC4Point<int32_t> lo(loX, loZ);
		return LayEdge(ctx, connType, hiX, hiZ, &lo, cells);
	}

	bool ClearEdgePiece(const MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, int32_t cells)
	{
		Logger& logger = Log();
		EdgeSetup e;
		const char* reason = "";
		if (!SetUpEdge(ctx, connType, bx, bz, cells, e, reason))
		{
			logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: not cleared (%s).", reason);
			return false;
		}
		const EdgeStub& s = e.stub;
		const NetType netType = static_cast<NetType>(e.net);
		const uint32_t clear = ~(0xFFu << (s.side * 8));
		logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: type %u net %d connection (%d,%d) side %u edge cell (%d,%d)",
			connType, e.net, bx, bz, s.side, s.sx, s.sz);
		if (CountPieces(e.occ, s.sx, s.sz, netType) == 0)
		{
			logger.WriteLine(LogLevel::Info, "EdgeClear: no piece on the edge cell, nothing to re-solve.");
			return false;
		}
		// Dual lane: the re-solve uses the width-2 tool, so log the edge cells
		// two rows either side (covers lo-1..hi+1 whichever lane this is).
		const bool dual = IsDualConnectionType(connType);
		const Step along = AlongStep(s.side);
		const auto logRows = [&](const char* when) {
			constexpr const char* kRows[5] = { "-2", "-1", "+0", "+1", "+2" };
			for (int32_t k = -2; k <= 2; k++)
			{
				OutsideRow(e.occ, s.sx + k * along.x, s.sz + k * along.z, e.cells, netType, "EdgeClear", kRows[k + 2], when);
			}
		};
		if (dual) logRows("before");
		const auto clearSide = [&](uint32_t f) { return f & clear; };
		const uint32_t seen = UpdateEdgeFlags(e.occ, s.sx, s.sz, netType, clearSide);

		for (uint32_t i = 0; i < 4; i++)
		{
			if (!((seen >> (i * 8)) & 0x04u)) continue;
			const int32_t px = s.sx + kStepX[i], pz = s.sz + kStepZ[i];
			if (px < 0 || pz < 0 || px >= e.cells || pz >= e.cells)
			{
				logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: partner cell (%d,%d) outside the city, skipped.", px, pz);
				continue;
			}
			logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: partner cell (%d,%d)", px, pz);
			UpdateEdgeFlags(e.occ, px, pz, netType, clearSide);
		}

		logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: GetNetworkTool(%d, true) ...", e.net);
		logger.Flush();
		cISC4NetworkTool* rawTool = e.nm->GetNetworkTool(e.net, true);
		logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: GetNetworkTool=%p", static_cast<void*>(rawTool));
		if (!rawTool)
		{
			return false;
		}
		const ToolGuard guard(rawTool);
		logger.WriteLine(LogLevel::Info, "EdgeClear: Init ...");
		logger.Flush();
		const bool inited = guard.tool->Init();
		logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: Init=%d", inited ? 1 : 0);
		const EdgeStub one{ s.sx, s.sz, s.sx, s.sz, s.side };
		logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: funds before=%lld", Funds(e.city));
		const bool placed = Place(guard.tool, one, "one-cell re-solve");
		logger.WriteLineFormatted(LogLevel::Info, "EdgeClear: funds after=%lld", Funds(e.city));
		CountPieces(e.occ, s.sx, s.sz, netType);
		if (dual) logRows("after");
		return placed;
	}
}
