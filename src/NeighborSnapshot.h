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
#include <array>
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
		int32_t otherTileX = 0;   // tile on the other side
		int32_t otherTileZ = 0;
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
		uint32_t schemaVersion = 2;
		int64_t writtenUnixTime = 0;   // wall clock on the writing machine
		int32_t simDateNumber = 0;     // cISC4Simulator::GetSimDateNumber of the writer
		uint32_t simYear = 0;
		uint32_t simMonth = 0;
		uint32_t simDay = 0;

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
	};

	// Slice 1 mirrors single-cell networks only: rail, road, power, pipe,
	// subway, light rail, dirt road, monorail.
	constexpr bool IsMirrorableConnectionType(uint32_t type)
	{
		switch (type)
		{
		case 1: case 2: case 4: case 5: case 6: case 7: case 10: case 11: return true;
		default: return false;
		}
	}

	constexpr uint32_t kDealTypeFirst = 0x4773FFE0;
	constexpr uint32_t kDealTypeLast = 0x4773FFE5;
	constexpr uint32_t kDealFlagCancelled = 0x4;
	constexpr uint32_t kDealFlagSilent = 0x80;
	constexpr uint32_t kDealFlagsKnown = 0x3FF & ~kDealFlagSilent;

	constexpr bool IsDealType(uint32_t type) { return type >= kDealTypeFirst && type <= kDealTypeLast; }
	// Even = buy, odd = sell; the other side's view of a deal flips the low bit.
	constexpr uint32_t OtherSideDealType(uint32_t type) { return type ^ 1; }
	constexpr uint32_t DealCommodity(uint32_t type) { return (type - kDealTypeFirst) / 2; }
	constexpr bool IsValidDealAmount(int32_t amount) { return amount >= 0 && amount <= 10000000; }
	constexpr bool IsValidExpiresIn(int32_t days) { return days >= 0 && days <= 36500; }
	constexpr uint32_t MaskDealFlags(uint32_t flags) { return flags & kDealFlagsKnown; }

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

	// Simple line-oriented key=value text format. No external JSON dependency,
	// trivially diffable, and readable by a PowerShell one-liner.
	bool WriteSnapshotFile(const std::filesystem::path& path, const NeighborSnapshot& snapshot, std::string& error);
	// On success, error holds any non-fatal problem (a list count that failed
	// to parse or was truncated); the caller logs it when non-empty.
	bool ReadSnapshotFile(const std::filesystem::path& path, NeighborSnapshot& snapshot, std::string& error);

	// "<x>_<z>.snapshot"
	std::string SnapshotFileName(int32_t regionX, int32_t regionZ);
}
