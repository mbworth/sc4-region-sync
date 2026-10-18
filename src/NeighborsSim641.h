/*
 * sc4-region-sync
 *
 * NeighborsSim641.h - FindDeals/FindDealsAll/ProposeDeals on
 * cISC4NeighborsSimulator with the element type the 1.1.641 binary actually
 * fills. The vendored header declares SC4List<cISC4NeighborConnection*>, but
 * the lists hold cISC4NeighborDeal* (docs/research/deals-641.md). Same slots,
 * same list layout; only the element type differs.
 *
 * GetDealCost (slot 35) and GetDealMaxAmount (slot 36) return double in ST0,
 * not int64_t; they are called through the vtable with that signature.
 */

#pragma once
#include "cISC4NeighborDeal.h"
#include "cISC4NeighborsSimulator.h"
#include "SC4List.h"
#include <cstddef>
#include <cstdint>

namespace regionsync::ns641
{
	inline void FindDealsAll(cISC4NeighborsSimulator* p, SC4List<cISC4NeighborDeal*>& deals)
	{
		p->FindDealsAll(reinterpret_cast<SC4List<cISC4NeighborConnection*>&>(deals));
	}

	inline void FindDeals(cISC4NeighborsSimulator* p, int32_t dealType, cISC4RegionalCity* pOther, SC4List<cISC4NeighborDeal*>& deals)
	{
		p->FindDeals(dealType, pOther, reinterpret_cast<SC4List<cISC4NeighborConnection*>&>(deals));
	}

	// Slots counted from cIGZUnknown (0 = QueryInterface).
	enum Slot : size_t
	{
		// 25 ProposeDeals and 34 UpdateCommodityStats go through the header.
		kGetDealCost = 35,       // double (int32 type, double amount, record const*)
		kGetDealMaxAmount = 36,  // double (int32 type, record const*)
	};

	// addToSim=false only returns the deals (what the advisor does).
	inline bool ProposeDeals(cISC4NeighborsSimulator* p, SC4List<cISC4NeighborDeal*>& deals, bool addToSim)
	{
		return p->ProposeDeals(reinterpret_cast<SC4List<cISC4NeighborConnection*>&>(deals), addToSim);
	}

	using GetDealCostFn = double(__thiscall*)(cISC4NeighborsSimulator*, int32_t, double, const cISC4RegionalCity*);
	using GetDealMaxAmountFn = double(__thiscall*)(cISC4NeighborsSimulator*, int32_t, const cISC4RegionalCity*);

	template <typename Fn>
	Fn At(cISC4NeighborsSimulator* p, Slot slot)
	{
		return reinterpret_cast<Fn>((*reinterpret_cast<void***>(p))[slot]);
	}

	inline double GetDealCost(cISC4NeighborsSimulator* p, int32_t dealType, double amount, const cISC4RegionalCity* pOther)
	{
		return At<GetDealCostFn>(p, kGetDealCost)(p, dealType, amount, pOther);
	}

	inline double GetDealMaxAmount(cISC4NeighborsSimulator* p, int32_t dealType, const cISC4RegionalCity* pOther)
	{
		return At<GetDealMaxAmountFn>(p, kGetDealMaxAmount)(p, dealType, pOther);
	}
}
