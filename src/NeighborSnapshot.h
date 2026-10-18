/*
 * sc4-region-sync - a SimCity 4 DLL plugin that keeps the region in sync
 * across multiple running game instances.
 *
 * NeighborSnapshot.h
 *
 * The small, plain-data summary of a city that crosses the border between
 * game instances. It mirrors the fields the game itself keeps in
 * cISC4RegionalCity for every tile in the region: the values a neighbor
 * reads when it needs to know about this city, without loading it.
 *
 * This file has no game dependencies on purpose so the format can be
 * unit-tested outside the game and read by other tools.
 */

#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace regionsync
{
	// The game indexes commodities by a small integer. The exact meaning of
	// each index is one of the things the Dump cheat exists to confirm; see
	// docs/OPEN-QUESTIONS.md. We carry enough slots to cover power, water,
	// garbage and whatever else the game tracks.
	constexpr size_t kCommoditySlots = 8;

	struct CommodityStats
	{
		float exported = 0.0f;
		float imported = 0.0f;
		float produced = 0.0f;
		float demanded = 0.0f;
		float costPerUnit = 0.0f;
	};

	// Population buckets the game reports per RCI group through
	// cISC4RegionalCity::GetPopulation(uint32_t type). The type IDs are the
	// standard SC4 RCI group IDs used by the demand simulator.
	struct RCIGroupPopulation
	{
		uint32_t groupId = 0;
		int32_t population = 0;
	};

	constexpr size_t kRCIGroupSlots = 12;

	// Schema 2. Connections and deals as the publisher sees them.
	struct ConnectionRecord
	{
		uint32_t type = 0;        // cISC4NeighborsSimulator::ConnectionType
		int32_t innerX = 0;       // inner edge cell, publisher's frame
		int32_t innerZ = 0;
		int32_t cellX = 0;        // GetConnectionCell: just outside the edge, publisher's frame
		int32_t cellZ = 0;
		bool cellKnown = false;   // false for snapshots written before the cell keys
		int32_t otherTileX = 0;   // tile on the other side
		int32_t otherTileZ = 0;
		bool active = false;      // display only (Dump); not serialized
	};

	struct DealRecord
	{
		uint32_t type = 0;        // 0x4773FFE0..E5, publisher's view
		int32_t otherTileX = 0;
		int32_t otherTileZ = 0;
		int32_t amount = 0;
		uint32_t flags = 0;       // without 0x80
		int32_t expiresIn = 0;    // days: expiration - current
		// Display only (Dump); not serialized.
		int32_t amountUsed = 0;
		int64_t cost = 0;
	};

	// Upper bound on list lengths read from a file (untrusted input).
	constexpr size_t kMaxListEntries = 4096;

	// Schema 3. The process that wrote a snapshot. Empty host, pid 0 or
	// start 0 means unknown (older snapshot or a bad value).
	struct WriterIdentity
	{
		std::string host;      // computer name, UTF-8
		uint32_t pid = 0;
		uint64_t start = 0;    // process creation time, FILETIME as 64 bits
		bool Known() const { return !host.empty() && pid != 0 && start != 0; }
	};

	constexpr size_t kMaxWriterHostLength = 255;

	// Non-empty, bounded, no control characters (keeps the line format intact).
	constexpr bool IsValidWriterHost(std::string_view host)
	{
		if (host.empty() || host.size() > kMaxWriterHostLength) return false;
		for (char c : host)
		{
			if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
		}
		return true;
	}

	struct NeighborSnapshot
	{
		// Identity
		int32_t regionX = 0;
		int32_t regionZ = 0;
		int32_t sizeX = 0;
		int32_t sizeZ = 0;
		uint32_t citySerial = 0;
		std::string cityName;
		bool established = false;

		// Provenance
		uint32_t schemaVersion = 3;
		int64_t writtenUnixTime = 0;   // wall clock on the writing machine
		int32_t simDateNumber = 0;     // cISC4Simulator::GetSimDateNumber of the writer
		uint32_t simYear = 0;
		uint32_t simMonth = 0;
		uint32_t simDay = 0;
		WriterIdentity writer;
		uint64_t cityLoadedAt = 0;   // wall clock (FILETIME) when the publisher loaded its city; 0 unknown

		// Summary the demand/neighbor simulators consume
		int32_t population = 0;
		int32_t commercialJobs = 0;
		int32_t industrialJobs = 0;
		float workforcePercentage = 0.0f;
		int8_t mayorRating = 0;
		float budget = 0.0f;
		float income = 0.0f;

		std::array<CommodityStats, kCommoditySlots> commodities{};
		std::array<RCIGroupPopulation, kRCIGroupSlots> rciGroups{};
		size_t rciGroupCount = 0;

		// Unknown (count key absent) is not the same as empty.
		bool connectionsKnown = false;
		std::vector<ConnectionRecord> connections;
		bool dealsKnown = false;
		std::vector<DealRecord> deals;
		// Every entry captured and the publisher's load reconcile done: only
		// then may a missing entry read as a removal. Absent reads false.
		bool connectionsComplete = false;
		bool dealsComplete = false;
	};

	// Everything but street, one-way road and ferry (docs/PLAN-DUAL-FERRY.md rule 11).
	constexpr bool IsMirrorableConnectionType(uint32_t type)
	{
		switch (type)
		{
		case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 12: return true;
		default: return false;
		}
	}

	// One connection per lane, two lanes per edge crossing.
	constexpr bool IsDualConnectionType(uint32_t type) { return type == 3 || type == 8 || type == 12; }

	constexpr uint32_t kDealTypeFirst = 0x4773FFE0;
	constexpr uint32_t kDealTypeLast = 0x4773FFE5;
	constexpr uint32_t kDealFlagProposed = 0x1;
	constexpr uint32_t kDealFlagActive = 0x2;
	constexpr uint32_t kDealFlagCancelled = 0x4;
	constexpr uint32_t kDealFlagSilent = 0x80;
	constexpr uint32_t kDealFlagsAmountLocked = 0x300;   // grace or probation: SetAmount is ignored
	constexpr uint32_t kDealFlagsDead = 0x1C;            // cancelled, no connection, cannot afford
	constexpr uint32_t kDealFlagsKnown = 0x3FF & ~kDealFlagSilent;

	constexpr bool IsDealType(uint32_t type) { return type >= kDealTypeFirst && type <= kDealTypeLast; }
	// Even = buy, odd = sell; the other side's view of a deal flips the low bit.
	constexpr uint32_t OtherSideDealType(uint32_t type) { return type ^ 1; }
	constexpr uint32_t DealCommodity(uint32_t type) { return (type - kDealTypeFirst) / 2; }
	constexpr bool IsValidDealAmount(int32_t amount) { return amount >= 0 && amount <= 10000000; }
	constexpr bool IsValidExpiresIn(int32_t days) { return days >= 0 && days <= 36500; }
	constexpr uint32_t MaskDealFlags(uint32_t flags) { return flags & kDealFlagsKnown; }
	// An offer not yet accepted: neither created nor removed by the mirror.
	constexpr bool IsProposedOnlyDeal(uint32_t flags) { return (flags & kDealFlagProposed) && !(flags & kDealFlagActive); }

	// The connection cell lies just outside one edge of A (cellsA wide), one
	// step outward from the inner cell; the other axis equals the inner cell's
	// and is inside [0, cellsA-1].
	constexpr bool IsConnectionCellAdjacent(int32_t innerX, int32_t innerZ, int32_t cellX, int32_t cellZ, int32_t cellsA)
	{
		const auto in = [cellsA](int32_t v) { return v >= 0 && v < cellsA; };
		const bool xEdge = (cellX == -1 && innerX == 0) || (cellX == cellsA && innerX == cellsA - 1);
		const bool zEdge = (cellZ == -1 && innerZ == 0) || (cellZ == cellsA && innerZ == cellsA - 1);
		return (xEdge && cellZ == innerZ && in(cellZ)) || (zEdge && cellX == innerX && in(cellX));
	}

	constexpr int32_t kCellsPerRegionUnit = 64;
	constexpr int32_t CityCells(uint32_t sizeEnum) { return kCellsPerRegionUnit << sizeEnum; }

	// Converts a publisher's inner edge cell into the subscriber's frame:
	// b = (a.region*64 + inner) - b.region*64 per axis. Valid only when the
	// result sits just outside one edge of the subscriber (-1 or bCells on
	// exactly one axis) and inside [0, bCells-1] on the other. Inputs come
	// from files: inner cells outside [-1, 256] or region coordinates outside
	// [0, 1024] are rejected before any arithmetic (bx = bz = 0).
	constexpr bool ToSubscriberFrame(
		int32_t aRegionX, int32_t aRegionZ, int32_t innerX, int32_t innerZ,
		int32_t bRegionX, int32_t bRegionZ, int32_t bCells,
		int32_t& bx, int32_t& bz)
	{
		bx = bz = 0;
		const auto cellOk = [](int32_t v) { return v >= -1 && v <= 256; };
		const auto regionOk = [](int32_t v) { return v >= 0 && v <= 1024; };
		if (!cellOk(innerX) || !cellOk(innerZ) || !regionOk(aRegionX) || !regionOk(aRegionZ) || !regionOk(bRegionX) || !regionOk(bRegionZ))
		{
			return false;
		}
		bx = (aRegionX * kCellsPerRegionUnit + innerX) - bRegionX * kCellsPerRegionUnit;
		bz = (aRegionZ * kCellsPerRegionUnit + innerZ) - bRegionZ * kCellsPerRegionUnit;
		const bool xEdge = bx == -1 || bx == bCells;
		const bool zEdge = bz == -1 || bz == bCells;
		const bool xIn = bx >= 0 && bx < bCells;
		const bool zIn = bz >= 0 && bz < bCells;
		return (xEdge && zIn) || (zEdge && xIn);
	}

	// The publisher's connection cell (just outside its edge, its frame) from
	// its inner cell and our cell (bx,bz) just outside our edge: one step from
	// the inner cell toward us. False when (bx,bz) is not just outside a
	// bCells-wide edge.
	constexpr bool PublisherConnectionCell(int32_t innerX, int32_t innerZ, int32_t bx, int32_t bz, int32_t bCells, int32_t& cellX, int32_t& cellZ)
	{
		cellX = innerX;
		cellZ = innerZ;
		const auto in = [bCells](int32_t v) { return v >= 0 && v < bCells; };
		if (bx == -1 && in(bz)) cellX = innerX + 1;
		else if (bx == bCells && in(bz)) cellX = innerX - 1;
		else if (bz == -1 && in(bx)) cellZ = innerZ + 1;
		else if (bz == bCells && in(bx)) cellZ = innerZ - 1;
		else return false;
		return true;
	}

	// Connection type to cISC4NetworkOccupant::eNetworkType; -1 when the edge
	// piece is not laid for that type (ferry, unknown).
	constexpr int32_t ConnectionToNetworkType(uint32_t connType)
	{
		switch (connType)
		{
		case 1: return 1;    // rail
		case 2: return 0;    // road
		case 3: return 2;    // elevated highway
		case 4: return 5;    // power
		case 5: return 4;    // pipe
		case 6: return 7;    // subway
		case 7: return 8;    // light rail
		case 8: return 6;    // avenue
		case 10: return 11;  // dirt road
		case 11: return 9;   // monorail
		case 12: return 12;  // ground highway
		default: return -1;
		}
	}

	// 3-cell stub from the edge cell (s) inward to (e). side: 0 west, 1 north,
	// 2 east, 3 south (the edge flag byte).
	struct EdgeStub
	{
		int32_t sx = 0, sz = 0, ex = 0, ez = 0;
		uint32_t side = 0;
	};

	// (bx,bz) is the cell just outside the edge (ToSubscriberFrame). Exactly
	// one axis must be -1 or cells, the other inside [0, cells-1].
	constexpr std::optional<EdgeStub> EdgeStubGeometry(int32_t bx, int32_t bz, int32_t cells)
	{
		if (cells < 3) return std::nullopt;
		const int32_t w = cells - 1;
		const auto in = [cells](int32_t v) { return v >= 0 && v < cells; };
		if (bx == -1 && in(bz)) return EdgeStub{ 0, bz, 2, bz, 0 };
		if (bz == -1 && in(bx)) return EdgeStub{ bx, 0, bx, 2, 1 };
		if (bx == cells && in(bz)) return EdgeStub{ w, bz, w - 2, bz, 2 };
		if (bz == cells && in(bx)) return EdgeStub{ bx, w, bx, w - 2, 3 };
		return std::nullopt;
	}

	// Dual lanes paired in the publisher's frame (docs/PLAN-DUAL-FERRY.md rule 2).
	// lanes: the publisher's complete list, unfiltered; cellsA: its tile width.
	// Per (type, publisher edge side), sorted along the edge and deduped, lanes
	// pair greedily from the lowest when one cell apart. Returns (lo, hi)
	// indices into lanes. A lane whose side is unknown (cell unknown and
	// inner cell on a corner) never pairs.
	std::vector<std::pair<size_t, size_t>> PairDualLanes(const std::vector<ConnectionRecord>& lanes, int32_t cellsA);

	// Marker direction for a dual lane on our side (0 W, 1 N, 2 E, 3 S), as the
	// game's load path computes it (dual-ferry-641.md section 2).
	constexpr bool DualLaneDir(uint32_t side, bool hi, bool driveOnRight)
	{
		return hi ^ (side == 1 || side == 2) ^ !driveOnRight;
	}

	// Simple line-oriented key=value text format. No external JSON dependency,
	// trivially diffable, and readable by a PowerShell one-liner.
	bool WriteSnapshotFile(const std::filesystem::path& path, const NeighborSnapshot& snapshot, std::string& error);
	// On success, error holds any non-fatal problem (a list count that failed
	// to parse or was truncated); the caller logs it when non-empty.
	bool ReadSnapshotFile(const std::filesystem::path& path, NeighborSnapshot& snapshot, std::string& error);

	// "<x>_<z>.snapshot"
	std::string SnapshotFileName(int32_t regionX, int32_t regionZ);
}
