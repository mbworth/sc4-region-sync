/*
 * sc4-region-sync
 *
 * NeighborsSim641.h - FindDeals/FindDealsAll on cISC4NeighborsSimulator
 * with the element type the 1.1.641 binary actually fills. The vendored
 * header declares SC4List<cISC4NeighborConnection*>, but the lists hold
 * cISC4NeighborDeal* (docs/research/deals-641.md). Same slots, same list
 * layout; only the element type differs.
 */

#pragma once
#include "cISC4NeighborDeal.h"
#include "cISC4NeighborsSimulator.h"
#include "SC4List.h"
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
}
