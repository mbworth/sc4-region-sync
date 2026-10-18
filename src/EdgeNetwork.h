/*
 * sc4-region-sync
 *
 * EdgeNetwork.h - lays the 3-cell network stub under a mirrored connection
 * (docs/PLAN-EDGE-TILE.md). Game-facing; never touches the message system.
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

	// The game's add=0 step after DeleteConnection (edge-tile-641.md): clear
	// the side byte on the edge cell (and its 0x04 partner), then re-solve
	// that one cell. Pieces stay. True when the re-solve returned nonzero.
	bool ClearEdgePiece(const MirrorContext& ctx, uint32_t connType, int32_t bx, int32_t bz, int32_t cells);
}
