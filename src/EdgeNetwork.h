/*
 * sc4-region-sync
 *
 * EdgeNetwork.h - lays the 3-cell network stub under a mirrored connection
 * (docs/PLAN-EDGE-TILE.md), 2 by 3 under a dual pair (docs/PLAN-DUAL-FERRY.md).
 * Game-facing; never touches the message system.
 */

#pragma once
#include "NeighborMirror.h"
#include <cstdint>

namespace regionsync
{
	struct EdgeLayResult
	{
		bool laid = false;
		const char* reason = "";
	};

	// (bx,bz) is the connection cell just outside our edge; cells is our tile width.
	EdgeLayResult LayEdgePiece(const MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, int32_t cells);

	// Dual pair (docs/PLAN-DUAL-FERRY.md rule 8): lo is one cell below hi along
	// the same edge; one placement over hi's line lays both lanes. laid = both
	// edge cells hold a piece. Never deletes a connection.
	EdgeLayResult LayEdgePair(const MirrorContext& ctx, uint32_t connType, int32_t loX, int32_t loZ, int32_t hiX, int32_t hiZ, int32_t cells);

	// The game's add=0 step after DeleteConnection (edge-tile-641.md): clear
	// the side byte on the edge cell (and its 0x04 partner), then re-solve
	// that one cell. Pieces stay. True when the re-solve returned nonzero.
	bool ClearEdgePiece(const MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, int32_t cells);
}
